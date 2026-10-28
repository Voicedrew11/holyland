// ---- The back end: the native GS's calls into Vulkan, made on a thread of their own ------------------
// (included in gsn.cpp's namespace; docs/port/RENDERER.md, "The back end")
//
// The GS's thread works out what to draw and writes each call into a stream (Cmd: a Recording of every call, with
// the images and buffers it names held). The back end's thread makes the calls of each stream sent, in the order
// sent, through Granite into its command buffer, submits them (OP_SUBMIT), begins Granite's next frame context
// (OP_NEXT_FRAME) and hands the pictures among them to the presenter (OP_PUBLISH, OP_PRESENT). A picture between made
// again from its recording is one call in the stream (OP_REPLAY). Granite's recording of the draws and the driver's
// work under it, which was nearly half of the GS's thread's time (a replay nearly all of it), is then done beside the
// GS's next work instead of before it.
//
// What waits for the GPU (a read-back: submit(true)) waits for its submit to be made first (backend_wait); what wants
// everything done (ring_idle) drains the back end too (backend_drain). The back end's thread has Granite's thread
// index 1, so that its command buffers and the GS's thread's (Granite's own, for a buffer made with data) come from
// pools of their own. KF4_GSN_BACKEND=0, or a Vulkan context of one thread index (paraLLEl-GS's, shared with
// --renderer both): no thread, each stream made at once on the thread that sends it (the calls as before the back
// end, in the same order).

struct Backend {
    // (shared: under lock)
    std::mutex lock;
    std::condition_variable wake, idle;
    std::deque<std::unique_ptr<Recording>> queue;  // the streams sent, in order
    std::vector<std::unique_ptr<Recording>> spare; // streams made and cleared, to be written again
    uint64_t sent = 0, made = 0;
    bool threaded = false, stop = false, stopped = false;
    // (the GS's thread's: the stream being written, and the streams sent before it)
    std::unique_ptr<Recording> writing;
    uint64_t sends = 0;
    // (the thread making the calls: the back end's, or the sender's without one)
    CommandBufferHandle cmd;
    uint64_t cmd_serial = 0;
    QueryPoolHandle gpu_last;
    uint64_t gpu_last_serial = 0;
    // (Shift+P's profiler) the GPU's intervals not read yet: Granite fills in their timestamps at the frame context
    // that finds their work done (on this thread); and this thread's waits for the GPU there
    std::deque<std::pair<QueryPoolHandle, QueryPoolHandle>> profile_intervals;
    uint64_t profile_wait_ns = 0;
    uint64_t binds_at_record = 0;
    // (--stats, added to gsn_stats by backend_drain)
    uint64_t ns_make = 0, record_binds = 0, replay_binds = 0; // (ns_make: less the waits at frame contexts)
};
Backend backend;

// A stream this long is sent before its submit, so that the back end starts on it while the GS goes on
const size_t backend_send_ops = 4096;
// and the GS's thread waits when this many are sent and not made: the back end is behind. Kept small, so that the
// GS's thread is never pictures ahead (its pictures' scanout slots, SCANOUT_SLOTS, are chosen before they are
// published) and the frame rate's fallback sees the back end's lateness as the GS's (gs.c: pictures between cut)
const size_t backend_queue_most = 6;

Recording &backend_stream() {
    Backend &b = backend;
    if (!b.writing) {
        std::lock_guard<std::mutex> hold(b.lock);
        if (!b.spare.empty()) {
            b.writing = std::move(b.spare.back());
            b.spare.pop_back();
        }
    }
    if (!b.writing) {
        b.writing = std::make_unique<Recording>();
    }
    return *b.writing;
}

uint64_t backend_sends() {
    return backend.sends;
}

bool backend_stream_long() {
    return backend.writing && backend.writing->ops.size() >= backend_send_ops;
}

// ---- Making the calls (the back end's thread) ----

// A picture between's calls made again: its job (the recording, the buffers its kicks' passes read, the targets'
// images now), and what its calls have bound
struct Replaying {
    const ReplayJob &job;
    // What the calls made again have bound (set, binding: buffer and offset), as Granite has it: a binding the same
    // as the one there is not made again (Granite writes the set's descriptors again for any buffer bound, the same
    // or not). Granite forgets its bindings at a render pass's beginning and end; the kicks' pass binds its own.
    struct Bound {
        uint64_t cookie;
        VkDeviceSize offset;
    } bound[3][8] = {};
    int gpu_cur = -1; // (KF4_GSN_GPU_TIME=2: what its calls draw since their last mark)

    explicit Replaying(const ReplayJob &j) : job(j) {}
    void forget() {
        memset(bound, 0, sizeof(bound));
    }
    bool same(uint32_t set, uint32_t binding, const Buffer *buffer, VkDeviceSize offset) {
        if (set >= 3 || binding >= 8) {
            return false;
        }
        Bound &b = bound[set][binding];
        if (b.cookie == buffer->get_cookie() && b.offset == offset) {
            return true;
        }
        b = {buffer->get_cookie(), offset};
        return false;
    }
    const Image &now_image(const Image *image) const {
        for (const auto &p : job.images) {
            if (p.first == image) {
                return *p.second;
            }
        }
        return *image;
    }
    const ImageView &now_view(const ImageView *view) const {
        const Image &image = view->get_image();
        const Image &now = now_image(&image);
        return &now == &image ? *view : now.get_view();
    }
};

// A timestamp: the GPU's time since the one before (in the same command buffer) is `kind`'s (KF4_GSN_GPU_TIME)
// (Shift+P's profiler) the intervals whose timestamps are in, added to the GPU's time; at most this many kept waiting
const size_t profile_intervals_most = 4096;
void profile_gpu_intervals() {
    Backend &b = backend;
    double period_ns = double(gs->device->get_gpu_properties().limits.timestampPeriod);
    while (!b.profile_intervals.empty()) {
        const auto &i = b.profile_intervals.front();
        if (!i.first->is_signalled() || !i.second->is_signalled()) {
            break;
        }
        uint64_t t0 = i.first->get_timestamp_ticks(), t1 = i.second->get_timestamp_ticks();
        if (t1 > t0) {
            port_profile_add(PROFILE_GPU, uint64_t(double(t1 - t0) * period_ns));
        }
        b.profile_intervals.pop_front();
    }
    while (b.profile_intervals.size() > profile_intervals_most) {
        b.profile_intervals.pop_front();
    }
}

// granite: the interval also to Granite's log of times (KF4_GSN_GPU_TIME)
void backend_timestamp(int kind, bool granite) {
    Backend &b = backend;
    QueryPoolHandle ts = b.cmd->write_timestamp(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    if (b.gpu_last && b.gpu_last_serial == b.cmd_serial) {
        if (granite) {
            gs->device->register_time_interval("gsn", b.gpu_last, ts, std::string("gsn ") + gpu_kinds[kind]);
            gpu_marks[kind]++;
        }
        if (port_profile_on()) {
            b.profile_intervals.emplace_back(b.gpu_last, ts);
        }
    }
    b.gpu_last = ts;
    b.gpu_last_serial = b.cmd_serial;
    profile_gpu_intervals();
}

// One call into the command buffer: a stream's (replaying null), or a recording's made again
void make_call(CommandBuffer &c, const Recording &r, const Recording::Op &op, Replaying *replaying) {
    auto image = [&](const Image *i) -> const Image & { return replaying ? replaying->now_image(i) : *i; };
    auto view = [&](const ImageView *v) -> const ImageView & { return replaying ? replaying->now_view(v) : *v; };
    auto same = [&](uint32_t set, uint32_t binding, const Buffer *buffer, VkDeviceSize offset) {
        return replaying && replaying->same(set, binding, buffer, offset);
    };
    switch (op.kind) {
    case OP_BARRIER: {
        const ArgBarrier &a = r.args<ArgBarrier>(op);
        c.barrier(a.src_stage, a.src_access, a.dst_stage, a.dst_access);
        break;
    }
    case OP_BEGIN_PASS: {
        ArgPass a = r.args<ArgPass>(op);
        if (a.info.num_subpasses) {
            a.info.subpasses = a.subpasses;
        }
        for (unsigned i = 0; i < a.info.num_color_attachments; i++) {
            a.info.color_attachments[i] = &view(a.info.color_attachments[i]);
        }
        if (a.info.depth_stencil) {
            a.info.depth_stencil = &view(a.info.depth_stencil);
        }
        c.begin_render_pass(a.info);
        if (replaying) {
            replaying->forget();
        }
        break;
    }
    case OP_END_PASS:
        c.end_render_pass();
        if (replaying) {
            replaying->forget();
        }
        break;
    case OP_OPAQUE:
        c.set_opaque_state();
        break;
    case OP_PROGRAM:
        c.set_program(r.args<Program *>(op));
        break;
    case OP_TOPOLOGY:
        c.set_primitive_topology(VkPrimitiveTopology(r.args<ArgValues>(op).v[0]));
        break;
    case OP_CULL:
        c.set_cull_mode(VkCullModeFlags(r.args<ArgValues>(op).v[0]));
        break;
    case OP_DEPTH_TEST:
        c.set_depth_test(r.args<ArgValues>(op).v[0] != 0, r.args<ArgValues>(op).v[1] != 0);
        break;
    case OP_DEPTH_COMPARE:
        c.set_depth_compare(VkCompareOp(r.args<ArgValues>(op).v[0]));
        break;
    case OP_DEPTH_BOUNDS:
        Cmd::depth_bounds_raw(c, r.args<ArgValues>(op).v[0] != 0, r.args<ArgValues>(op).f[0], r.args<ArgValues>(op).f[1]);
        break;
    case OP_STENCIL:
        Cmd::stencil_raw(c, r.args<ArgValues>(op).v[0] != 0, VkCompareOp(r.args<ArgValues>(op).v[1]),
                         VkStencilOp(r.args<ArgValues>(op).v[2]), r.args<ArgValues>(op).v[3]);
        break;
    case OP_VIEWPORT:
        c.set_viewport(r.args<VkViewport>(op));
        break;
    case OP_SCISSOR:
        c.set_scissor(r.args<VkRect2D>(op));
        break;
    case OP_TEXTURE: {
        const ArgTexture &a = r.args<ArgTexture>(op);
        c.set_texture(a.set, a.binding, view(a.view), a.sampler);
        break;
    }
    case OP_CONSTANT: {
        const ArgPlaced &a = r.args<ArgPlaced>(op);
        if (!same(a.a, a.b, a.buffer, a.offset)) {
            c.set_uniform_buffer(a.a, a.b, *a.buffer, a.offset, (a.size + 15) & ~15u);
        }
        break;
    }
    case OP_BLEND_ENABLE:
        c.set_blend_enable(r.args<ArgValues>(op).v[0] != 0);
        break;
    case OP_BLEND_CONSTANTS:
        c.set_blend_constants(r.args<ArgValues>(op).f);
        break;
    case OP_BLEND_FACTORS: {
        const ArgValues &a = r.args<ArgValues>(op);
        c.set_blend_factors(VkBlendFactor(a.v[0]), VkBlendFactor(a.v[1]), VkBlendFactor(a.v[2]), VkBlendFactor(a.v[3]));
        break;
    }
    case OP_BLEND_OP:
        c.set_blend_op(VkBlendOp(r.args<ArgValues>(op).v[0]), VkBlendOp(r.args<ArgValues>(op).v[1]));
        break;
    case OP_VERTEX_DATA: {
        const ArgPlaced &a = r.args<ArgPlaced>(op);
        c.set_vertex_binding(a.a, *a.buffer, a.offset, a.b);
        break;
    }
    case OP_VERTEX_BINDING: {
        const ArgBinding &a = r.args<ArgBinding>(op);
        c.set_vertex_binding(a.binding, *a.buffer, a.offset, a.stride, a.rate);
        break;
    }
    case OP_VERTEX_ATTRIB: {
        const ArgAttrib &a = r.args<ArgAttrib>(op);
        c.set_vertex_attrib(a.attrib, a.binding, a.format, a.offset);
        break;
    }
    case OP_WRITE_MASK:
        c.set_color_write_mask(r.args<ArgValues>(op).v[0]);
        break;
    case OP_PUSH: {
        const ArgData &a = r.args<ArgData>(op);
        c.push_constants(r.arena.data() + op.at + sizeof(ArgData), a.a, a.size);
        break;
    }
    case OP_DRAW: {
        const ArgDraw &a = r.args<ArgDraw>(op);
        if (replaying && gpu_time >= 2) {
            // (a stream's draws are counted where Cmd writes them)
            int kind = replaying->gpu_cur >= 0 ? replaying->gpu_cur : GPU_UNTAGGED;
            gpu_draws[kind]++;
            gpu_tris[kind] += a.count / 3;
        }
        c.draw(a.count, a.instances, a.first, a.first_instance);
        break;
    }
    case OP_COPY_IMAGE: {
        const ArgCopy &a = r.args<ArgCopy>(op);
        c.copy_image(image(a.dst), image(a.src), a.dst_offset, a.src_offset, a.extent, a.dst_sub, a.src_sub);
        break;
    }
    case OP_UPDATE_IMAGE: {
        const ArgUpdate &a = r.args<ArgUpdate>(op);
        if (a.from) {
            // (a stream's bytes in the upload chunk; a recording's target's pixels from VRAM, already on the GPU:
            // finish_recording)
            c.copy_buffer_to_image(image(a.image), *a.from, a.from_offset, a.offset, a.extent, a.row_length,
                                   a.image_height, a.sub);
        }
        // (else a recording's texture decoded: its image is written once, and keeps it)
        break;
    }
    case OP_RESOLVE: {
        const ArgResolve &a = r.args<ArgResolve>(op);
        Cmd::resolve_raw(c, image(a.dst), image(a.src), a.region);
        break;
    }
    case OP_CLEAR_IMAGE: {
        const ArgClear &a = r.args<ArgClear>(op);
        c.clear_image(image(a.image), a.value);
        break;
    }
    case OP_INTERP_KICKS:
        if (replaying) {
            const ReplayJob &j = replaying->job;
            interp_kicks_pass(c, ArgKicks{j.picture.get(), j.kicks.get(), j.moves.get(), j.resolved.get(),
                                          j.corners.get(), j.vertices.get(), j.kick_count});
            replaying->forget();
        }
        break;
    case OP_KICKS:
        interp_kicks_pass(c, r.args<ArgKicks>(op));
        break;
    case OP_MEMORY_BARRIER: {
        struct A {
            VkMemoryBarrier2 b;
            VkDependencyFlags flags;
        };
        const A &a = r.args<A>(op);
        Cmd::memory_barrier_raw(c, a.b, a.flags);
        break;
    }
    case OP_INPUT_ATTACHMENTS:
        c.set_input_attachments(r.args<ArgValues>(op).v[0], r.args<ArgValues>(op).v[1]);
        break;
    case OP_STORAGE: {
        const ArgBinding &a = r.args<ArgBinding>(op);
        if (!same(a.binding & 0xFF, a.binding >> 8, a.buffer, 0)) {
            c.set_storage_buffer(a.binding & 0xFF, a.binding >> 8, *a.buffer);
        }
        break;
    }
    case OP_STORAGE_DATA: {
        const ArgPlaced &a = r.args<ArgPlaced>(op);
        if (!same(a.a, a.b, a.buffer, a.offset)) {
            c.set_storage_buffer(a.a, a.b, *a.buffer, a.offset, a.size);
        }
        break;
    }
    case OP_CONSERVATIVE:
        c.set_conservative_rasterization(r.args<ArgValues>(op).v[0] != 0);
        break;
    case OP_GPU_MARK:
        // (a recording's: gpu_mark's and gpu_enter's work again, with the replay's own kind under way)
        if (replaying) {
            const ArgValues &a = r.args<ArgValues>(op);
            int kind = int(a.v[0]);
            if (a.v[1]) {
                backend_timestamp(replaying->gpu_cur >= 0 ? replaying->gpu_cur : GPU_UNTAGGED, true); // (recorded: KF4_GSN_GPU_TIME=2)
                replaying->gpu_cur = kind;
            } else {
                if (gpu_time >= 2 && replaying->gpu_cur >= 0) {
                    kind = replaying->gpu_cur;
                    replaying->gpu_cur = -1;
                }
                backend_timestamp(kind, true);
            }
        }
        break;
    case OP_BLIT: {
        const ArgBlit &a = r.args<ArgBlit>(op);
        c.blit_image(image(a.dst), image(a.src), a.dst_offset, a.dst_extent, a.src_offset, a.src_extent, 0, 0, 0, 0, 1,
                     a.filter);
        break;
    }
    case OP_COPY_TO_BUFFER: {
        const ArgCopyToBuffer &a = r.args<ArgCopyToBuffer>(op);
        c.copy_image_to_buffer(*a.dst, *a.src, a.buffer_offset, a.offset, a.extent, a.row_length, a.slice_height, a.sub);
        break;
    }
    case OP_UPSCALE:
        upscale_call(c, r.args<ArgUpscale>(op));
        break;
    default:
        // (the stream's own calls: backend_make)
        break;
    }
}

// A picture between: its recording's calls made again into the command buffer
void replay_ops(CommandBuffer &c, const ReplayJob &job) {
    Replaying replaying(job);
    uint64_t binds0 = pipeline_binds();
    for (const Recording::Op &op : job.recording->ops) {
        make_call(c, *job.recording, op, &replaying);
    }
    backend.replay_binds += pipeline_binds() - binds0;
}

// A stream's calls
void backend_make(Recording &s) {
    Backend &b = backend;
    auto start = std::chrono::steady_clock::now();
    for (const Recording::Op &op : s.ops) {
        switch (op.kind) {
        case OP_NEW_CMD: {
            b.cmd = gs->device->request_command_buffer();
            b.cmd_serial++;
            DynamicState d = dynamic_state_choice();
            b.cmd->set_dynamic_state(d.ds, d.blend); // (kept through set_opaque_state: the command buffer's own)
            break;
        }
        case OP_SUBMIT: {
            SubmitWait *w = s.args<ArgSubmit>(op).wait;
            if (w) {
                gs->device->submit(b.cmd, &w->fence);
                std::lock_guard<std::mutex> hold(w->lock);
                w->done = true;
                w->cv.notify_all();
            } else {
                gs->device->submit(b.cmd);
            }
            b.cmd.reset();
            break;
        }
        case OP_NEXT_FRAME: {
            // (mostly a wait for the GPU's work of four vblanks before: not counted as the back end's work)
            auto wait = std::chrono::steady_clock::now();
            gs->device->next_frame_context();
            auto waited = std::chrono::steady_clock::now() - wait;
            start += waited;
            b.profile_wait_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(waited).count());
            upscale_next_frame();
            break;
        }
        case OP_PUBLISH: {
            const ArgPublish &a = s.args<ArgPublish>(op);
            scanout_published(a.slot, a.tag);
            break;
        }
        case OP_PRESENT:
            if (presenter_image) {
                presenter_image(s.args<ArgPublish>(op).tag);
            }
            break;
        case OP_TIMESTAMP:
            backend_timestamp(int(s.args<ArgValues>(op).v[0]), s.args<ArgValues>(op).v[1] != 0);
            break;
        case OP_REPLAY:
            replay_ops(*b.cmd, s.replays[s.args<ArgValues>(op).v[0]]);
            break;
        case OP_RECORDING_BINDS:
            if (s.args<ArgValues>(op).v[0]) {
                b.record_binds += pipeline_binds() - b.binds_at_record;
            } else {
                b.binds_at_record = pipeline_binds();
            }
            break;
        default:
            make_call(*b.cmd, s, op, nullptr);
            break;
        }
    }
    b.ns_make += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count());
}

void backend_thread() {
    Util::register_thread_index(1); // (Granite's command pools and descriptor sets: this thread's own)
    name_thread("kf4-gs-back");
    Backend &b = backend;
    for (;;) {
        std::unique_ptr<Recording> s;
        {
            std::unique_lock<std::mutex> hold(b.lock);
            b.wake.wait(hold, [&] { return !b.queue.empty() || b.stop; });
            if (b.queue.empty()) {
                b.stopped = true;
                b.idle.notify_all();
                return;
            }
            s = std::move(b.queue.front());
            b.queue.pop_front();
        }
        uint64_t t0 = port_profile_on() ? port_profile_now_ns() : 0, wait0 = b.profile_wait_ns;
        backend_make(*s);
        if (t0 != 0) {
            uint64_t wait = b.profile_wait_ns - wait0;
            port_profile_add(PROFILE_BACKEND_WAIT, wait);
            port_profile_add(PROFILE_BACKEND, port_profile_now_ns() - t0 - wait);
        }
        s->clear(); // (what it held let go here: Granite keeps each until the GPU is done with it)
        std::lock_guard<std::mutex> hold(b.lock);
        b.spare.push_back(std::move(s));
        b.made++;
        b.idle.notify_all();
    }
}

// ---- The GS's side ----

// The calls written so far, to the back end (or made now, without its thread)
void backend_send() {
    Backend &b = backend;
    if (!b.writing || b.writing->ops.empty()) {
        return;
    }
    b.sends++;
    std::unique_lock<std::mutex> hold(b.lock);
    if (!b.threaded || b.stopped) {
        hold.unlock();
        backend_make(*b.writing);
        b.writing->clear();
        return;
    }
    {
        ProfileTimer waiting(PROFILE_GS_WAIT);
        b.idle.wait(hold, [&] { return b.sent - b.made < backend_queue_most || b.stopped; });
    }
    if (b.stopped) {
        hold.unlock();
        backend_make(*b.writing);
        b.writing->clear();
        return;
    }
    b.queue.push_back(std::move(b.writing));
    b.sent++;
    b.wake.notify_one();
}

// Until the submit is made (sent: submit), then until the GPU is done with it
void backend_wait(SubmitWait &w) {
    ProfileTimer waiting(PROFILE_GS_WAIT);
    {
        std::unique_lock<std::mutex> hold(w.lock);
        w.cv.wait(hold, [&] { return w.done; });
    }
    w.fence->wait();
}

// Everything written, made; the back end's counts added to gsn_stats
void backend_drain() {
    Backend &b = backend;
    backend_send();
    std::unique_lock<std::mutex> hold(b.lock);
    {
        ProfileTimer waiting(PROFILE_GS_WAIT);
        b.idle.wait(hold, [&] { return b.made == b.sent || b.stopped; });
    }
    gsn_stats.ns_backend += b.ns_make;
    gsn_stats.interp_record_binds += b.record_binds;
    gsn_stats.interp_replay_binds += b.replay_binds;
    b.ns_make = b.record_binds = b.replay_binds = 0;
}

// At exit: what was sent made (two seconds at most), then the thread stops; anything sent after is made at once
void backend_stop() {
    Backend &b = backend;
    std::unique_lock<std::mutex> hold(b.lock);
    if (!b.threaded) {
        return;
    }
    b.idle.wait_for(hold, std::chrono::seconds(2), [&] { return b.made == b.sent; });
    b.stop = true;
    b.wake.notify_one();
    b.idle.wait_for(hold, std::chrono::seconds(2), [&] { return b.stopped; });
}

void backend_start() {
    const char *on = getenv("KF4_GSN_BACKEND");
    if ((on && atoi(on) == 0) || gs->context->get_num_thread_indices() < 2) {
        return;
    }
    backend.threaded = true;
    std::thread(backend_thread).detach();
    atexit(backend_stop);
}
