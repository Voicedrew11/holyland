// ---- Standalone (P8 T2): the scene at a scale of its own, and the composite -----------------------------
// (docs/port/NATIVE_DRAW_P8.md, T2.) Included by gsn.cpp after gsn_standalone.inc, inside its namespace.
//
// With --scene-scale (and --scene-msaa) the scene's targets have a scale and samples of their own, apart from the
// GS's (--native-scale, --native-msaa): the world can be drawn at 4x with 4 samples while the GS draws the HUD, the
// messages, the menus and the title at 1x, as the PS2 did. The scene gives the frame to the GS at the GS's scale
// (sa_give: each GS pixel the mean of the scene's pixels and samples over it), and the GS draws what follows the
// world over it as it always has, at its own scale, with its own arithmetic (the 2D, the fades, the brightness).
// The picture shown is then made from both (sa_composite, at the scanout): the scene's picture where the GS drew
// nothing over the world but colours over the whole screen, the GS's where it drew the 2D.
//
// Why not a transparent layer. The plan had the GS draw its 2D into a transparent layer, blended over the scene
// (premultiplied). The GS's 2D needs the frame's own alpha (the MP orb is drawn through a mask of destination
// alpha: FBA, then DATE), and what it draws after the world includes passes that read the whole frame (the
// brightness, last, over the HUD too; the tint under water) and fills over it (the fades, the message's dim, the
// pause): a layer's colour and coverage cannot carry those. Instead:
// - the scene's picture is kept as it was given (sa_hold: hi, the scene's, its samples resolved; lo, the GS's frame
//   just after, at the GS's scale);
// - each batch the GS then draws over that frame (sa_note_gs_batch) that is one colour over the whole screen is
//   kept in order, the chain: a flat colour blended over it (a fill, a fade, the pause's shade: ALPHA with As or
//   FIX), set over it (no blending), or the frame read as a texture a texel a pixel and modulated by a colour (the
//   brightness, the tint);
// - at the scanout each pixel of the picture looks at its GS pixel: where the GS's frame is what the chain makes of
//   lo (within a level or two: Vulkan's blending rounds as it rounds), nothing else was drawn there, and the
//   pixel is the scene's own, through the chain (with the GS's integer arithmetic); anywhere else the GS's 2D is
//   over it, and the pixel is the GS's (its GS pixel repeated, as the PS2's 2D is). Under the 2D's see-through
//   parts the world is the GS's scale.
// A frame kept is good for the vblanks that show it (the frame's own and the one after, and the pictures between
// them); the GS's clear of that buffer for the next frame, or a fourth vblank, lets it go.

struct SaHeld {
    bool valid = false;
    uint32_t fbp = 0, fbw = 0, fpsm = 0;
    const Target *t = nullptr;
    uint32_t sx = 1, sy = 1;   // the scene's scale
    uint32_t gsx = 1, gsy = 1; // the GS target's
    uint32_t rows = 0;         // the frame's rows the world was drawn in (GS pixels: the scissor's)
    ImageHandle hi, lo;
    ImageHandle up; // (--upscaler) hi brought up to the GS's scale, at the composite (sa_upscale_held)
    // (upscaling) what the picture's motion is worked out from: its depth, copied as hi is (by a picture between's
    // replays too), and a game frame's own camera, jitter (scene pixels) and XYOFFSET. A picture between's camera and
    // jitter are the GS thread's at its composite (sa_cam.picture): its recording holds once for all its replays.
    ImageHandle depth, motion, shown;
    ImageHandle fdepth; // (--upscaler fsr3) the depth as FSR takes it (sa_motion_pass)
    bool between = false;
    double world_screen[16] = {};
    float jitter[2] = {0, 0}, origin[2] = {0, 0};
    double zinf = 0;
    float zscale = 0;
    // (FSR's depth) the screen matrix's Z and w rows' z and 1 columns, the view's depth at the nearest Z, and the
    // view's height as an angle (radians)
    double zview[4] = {};
    float near_plane = 0, fov_y = 0;
    uint32_t ops[8][4] = {};
    int count = 0;
    uint64_t vblank = 0;
};
std::vector<SaHeld> sa_held;
Program *sa_program_composite = nullptr;
Program *sa_program_motion = nullptr;
// (upscaling) the last picture composited's world_screen: the motion is from it to the picture composited now
struct SaMotionLast {
    bool valid = false;
    double world_screen[16] = {};
} sa_motion_last;
// (--upscaler fsr3) The last picture upscaled: the picture's number (gsn_stats.pictures) and time
struct SaUpscaleLast {
    bool valid = false;
    uint64_t picture = 0;
    std::chrono::steady_clock::time_point at;
} sa_upscale_last;
// (set by the back end where FSR could not be had, gsn_upscale.inc: the composite goes back to bilinear; DLSS: to FSR)
std::atomic<bool> upscale_failed{false}, dlss_failed{false};

// --upscaler (docs/port/UPSCALING.md): a scene kept below the GS's scale is brought up to it at the composite, and the
// picture shown is at the GS's scale: the 2D the GS's, at its own scale, the world the upscaler's. Without it the
// composite shows a scene below the GS's scale at the scene's, the 2D brought down with it. The composite is also where
// the pictures come in the order they are shown, which a temporal upscaler needs: the GS draws a game frame's own
// picture before the pictures between that are shown before it (STEP3.md, "Higher frame rates"), but each picture is
// composited at its own scanout.
bool sa_held_upscaled(const SaHeld &h) {
    return sa_upscaler && h.sx <= h.gsx && h.sy <= h.gsy && (h.sx < h.gsx || h.sy < h.gsy);
}

SaHeld *sa_held_of(uint32_t fbp, uint32_t fbw, uint32_t fpsm, bool make) {
    for (SaHeld &h : sa_held) {
        if (h.fbp == fbp && h.fbw == fbw && h.fpsm == fpsm) {
            return &h;
        }
    }
    if (!make) {
        return nullptr;
    }
    if (sa_held.size() >= 4) {
        sa_held.erase(sa_held.begin());
    }
    sa_held.emplace_back();
    SaHeld &h = sa_held.back();
    h.fbp = fbp;
    h.fbw = fbw;
    h.fpsm = fpsm;
    return &h;
}

void sa_held_forget() {
    sa_held.clear();
}

// (sa_give, a frame given back for good at another scale or samples than the GS's) The scene's picture kept, and
// the GS's frame as it was given it
void sa_hold(Target &color) {
    SaHeld *h = sa_held_of(sa.fbp, sa.fbw, sa.fpsm, true);
    uint32_t W = sa.color->get_width(), H = sa.color->get_height();
    uint32_t gw = color.image->get_width(), gh = color.image->get_height();
    if (!h->hi || h->hi->get_width() != W || h->hi->get_height() != H || h->hi->get_format() != sa.color->get_format()) {
        h->hi = sa_image(W, H, sa.color->get_format(), 1);
    }
    if (!h->lo || h->lo->get_width() != gw || h->lo->get_height() != gh || h->lo->get_format() != color.image->get_format()) {
        h->lo = sa_image(gw, gh, color.image->get_format(), 1);
    }
    {
        Why why(GSN_WHY_COPY);
        end_pass();
    }
    gpu_mark(GPU_OTHER);
    if (sa.samples > 1) {
        msaa_resolve(*h->hi, *sa.color, {0, 0, 0}, {W, H, 1});
    } else {
        transfer_barrier();
        cmd().copy_image(*h->hi, *sa.color, {0, 0, 0}, {0, 0, 0}, {W, H, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        transfer_barrier();
    }
    {
        Why why(GSN_WHY_COPY);
        msaa_settle(color);
        end_pass();
    }
    transfer_barrier();
    cmd().copy_image(*h->lo, *color.image, {0, 0, 0}, {0, 0, 0}, {gw, gh, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    transfer_barrier();
    gpu_mark(GPU_COPY);
    gsn_stats.copy_bytes += (uint64_t(W) * H + uint64_t(gw) * gh) * (wide(*h->lo) ? 8 : 4);
    h->t = &color;
    h->sx = sa.sx;
    h->sy = sa.sy;
    h->gsx = color.sx;
    h->gsy = color.sy;
    h->rows = std::max(sa.scissor_rows, 1u);
    h->count = 0;
    h->valid = true;
    h->vblank = sa_vblanks;
    if (sa_motion_wanted() && sa.samples == 1) {
        // (upscaling) the depth, for the picture's motion at its composite; and a game frame's own camera and jitter
        if (!h->depth || h->depth->get_width() != W || h->depth->get_height() != H ||
            h->depth->get_format() != sa.depth->get_format()) {
            h->depth = sa_image(W, H, sa.depth->get_format(), 1);
        }
        transfer_barrier();
        cmd().copy_image(*h->depth, *sa.depth, {0, 0, 0}, {0, 0, 0}, {W, H, 1}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1},
                         {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1});
        transfer_barrier();
        h->between = interp.active;
        if (!interp.active) {
            memcpy(h->world_screen, sa_cam.world_screen, sizeof(h->world_screen));
            h->jitter[0] = sa_jitter_on() ? sa_jit.px[0] : 0.0f;
            h->jitter[1] = sa_jitter_on() ? sa_jit.px[1] : 0.0f;
        }
        memcpy(h->origin, sa_cam.origin, sizeof(h->origin));
        // (Z as the view's depth goes to infinity: the far plane, where the scene's depth is 0)
        h->zinf = sa_cam.screen[3 * 4 + 2] != 0.0 ? sa_cam.screen[2 * 4 + 2] / sa_cam.screen[3 * 4 + 2] : 0.0;
        h->zscale = float(depth_scale(sa.zpsm));
        // (FSR's depth: near / the view's depth, near the view's depth at the nearest Z the scene holds, 1 / zscale;
        // Z = (zview[0] vz + zview[1]) / (zview[2] vz + zview[3]))
        const double *sm = sa_cam.screen;
        double zv[4] = {sm[2 * 4 + 2], sm[2 * 4 + 3], sm[3 * 4 + 2], sm[3 * 4 + 3]};
        memcpy(h->zview, zv, sizeof(zv));
        double z1 = h->zscale > 0 ? 1.0 / h->zscale : 0.0, below = zv[2] * z1 - zv[0];
        double near_plane = below != 0.0 ? (zv[1] - zv[3] * z1) / below : 0.0;
        h->near_plane = near_plane > 0.0 ? float(near_plane) : 1.0f;
        // (the view's height: the scene's rows in the GS's pixels over the screen matrix's focal length down)
        double focal = zv[2] != 0.0 ? std::fabs(sm[1 * 4 + 1] / zv[2]) : 0.0;
        h->fov_y = focal > 0.0 ? float(2.0 * std::atan(0.5 * double(H) / double(sa.sy) / focal)) : 1.0f;
    } else {
        h->depth.reset();
    }
}

// (sa_upscale_held, upscaling) The held picture's motion from the last picture composited, a scene pixel at a time
// (shaders/sa_motion.frag), into h.motion (RG16F, the scene's pixels); or a picture of it to look at into h.shown
// (KF4_UPSCALE_SHOW=motion). The camera's motion only: the depth taken back through this picture's world_screen and
// forward through the last one's, one matrix made of the two here in doubles. Models move with the camera here. And
// the depth as FSR takes it, into h.fdepth (R32F).
void sa_motion_pass(SaHeld &h) {
    if (!sa_program_motion) {
        ResourceLayout vert = {};
        ResourceLayout frag = {};
        frag.output_mask = 3;
        frag.push_constant_size = 128;
        frag.sets[0].sampled_image_mask = 1;
        frag.sets[0].fp_mask = 1;
        frag.sets[0].meta[0].array_size = 1;
        sa_program_motion = gs->device->request_program(
            gs->device->request_shader(gsn_spv_msaa_vert, sizeof(gsn_spv_msaa_vert), &vert),
            gs->device->request_shader(gsn_spv_sa_motion_frag, sizeof(gsn_spv_sa_motion_frag), &frag));
    }
    const double *cur = h.between ? sa_cam.picture : h.world_screen;
    const float *jitter = h.between ? sa_cam.picture_jitter : h.jitter;
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}, inv[16];
    if (sa_motion_last.valid && sa_mat_inverse(inv, cur)) {
        sa_mat_mul(m, sa_motion_last.world_screen, inv);
    }
    memcpy(sa_motion_last.world_screen, cur, sizeof(sa_motion_last.world_screen));
    sa_motion_last.valid = sa_cam.valid;
    uint32_t W = h.depth->get_width(), H = h.depth->get_height();
    bool show = sa_motion_show();
    ImageHandle &out = show ? h.shown : h.motion;
    VkFormat format = show ? h.hi->get_format() : VK_FORMAT_R16G16_SFLOAT;
    if (!out || out->get_width() != W || out->get_height() != H || out->get_format() != format) {
        out = sa_image(W, H, format, 1);
    }
    if (!h.fdepth || h.fdepth->get_width() != W || h.fdepth->get_height() != H) {
        h.fdepth = sa_image(W, H, VK_FORMAT_R32_SFLOAT, 1);
    }
    struct {
        float m[16];        // column-major, as GLSL's mat4
        float inv_scale[2]; // GS pixels a scene pixel is
        float origin[2];
        float jitter[2];
        float zscale, zinf, show;
        float near_plane, pad[2];
        float zview[4];
    } push = {};
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            push.m[c * 4 + r] = float(m[r * 4 + c]);
        }
    }
    push.inv_scale[0] = 1.0f / float(h.sx);
    push.inv_scale[1] = 1.0f / float(h.sy);
    push.origin[0] = h.origin[0];
    push.origin[1] = h.origin[1];
    push.jitter[0] = jitter[0];
    push.jitter[1] = jitter[1];
    push.zscale = h.zscale;
    push.zinf = float(h.zinf);
    push.show = show ? 1.0f : 0.0f;
    push.near_plane = h.near_plane;
    for (int i = 0; i < 4; i++) {
        push.zview[i] = float(h.zview[i]);
    }
    static_assert(sizeof(push) == 128, "sa_motion.frag's Push");
    end_pass();
    RenderPassInfo rp;
    rp.num_color_attachments = 2;
    rp.color_attachments[0] = &out->get_view();
    rp.color_attachments[1] = &h.fdepth->get_view();
    rp.store_attachments = 3;
    Cmd &c = cmd();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    c.begin_render_pass(rp);
    c.set_opaque_state();
    c.set_program(sa_program_motion);
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_test(false, false);
    c.set_texture(0, 0, h.depth->get_view(), StockSampler::NearestClamp); // (Granite samples its depth view)
    c.push_constants(&push, 0, sizeof(push));
    c.draw(3);
    c.end_render_pass();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    sa_stats.motions++;
}

// (sa_flush, a batch the GS draws) The GS's clear of a frame (libgraph's, at the swap: untextured sprites of one
// colour and Z over the whole frame, Z always passing and written): the scene may take that frame by clearing its
// own (sa.clear), and a frame kept of that buffer is let go. Then, of a frame the scene gave back (kept: sa_hold),
// the batches drawn over the whole of it in one colour: the composite's chain
void sa_note_gs_batch() {
    if (gs->verts.empty()) {
        return;
    }
    uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
    bool tme = (prim >> 4) & 1, abe = (prim >> 6) & 1, fst = (prim >> 8) & 1;
    uint64_t frame = gs->reg[R_FRAME_1 + ctxt], zbuf = gs->reg[R_ZBUF_1 + ctxt], test = gs->reg[R_TEST_1 + ctxt];
    uint64_t scissor = gs->reg[R_SCISSOR_1 + ctxt], alpha = gs->reg[R_ALPHA_1 + ctxt];
    uint64_t tex0 = gs->reg[R_TEX0_1 + ctxt], tex1 = gs->reg[R_TEX1_1 + ctxt];
    uint32_t fbp = bits(frame, 0, 9) * 32, fbw = bits(frame, 16, 6), fpsm = bits(frame, 24, 6);
    uint32_t zbp = bits(zbuf, 0, 9) * 32, zpsm = bits(zbuf, 24, 4) | 0x30;
    uint32_t rows = bits(scissor, 48, 11) + 1;
    bool zte = bits(test, 16, 1), date = bits(test, 14, 1);
    uint32_t ztst = zte ? bits(test, 17, 2) : 1;
    bool ate = bits(test, 0, 1) && bits(test, 1, 3) != 1;
    bool from_origin = bits(scissor, 0, 11) == 0 && bits(scissor, 32, 11) == 0;
    uint32_t rgba = 0;
    if (!tme && !abe && zte && ztst == 1 && !bits(zbuf, 32, 1) && !date && !ate && uint32_t(frame >> 32) == 0 && from_origin &&
        bits(scissor, 16, 11) + 1 >= fbw * 64 && sa_sprites_cover(fbw, rows, &rgba)) {
        bool one_z = true;
        for (const Vtx &v : gs->verts) {
            one_z &= v.z == gs->verts[0].z;
        }
        if (one_z) {
            sa.clear = {true, fbp, fbw, fpsm, zbp, zpsm, rgba, gs->verts[0].z, rows};
            if (SaHeld *h = sa_held_of(fbp, fbw, fpsm, false)) {
                h->valid = false;
            }
            if (sa.log) {
                fprintf(stderr, "[sa] the GS's clear: fbp %#x zbp %#x colour %#x z %u, %u rows\n", fbp, zbp, rgba,
                        gs->verts[0].z, rows);
            }
            return;
        }
    }
    const Sa::Clear &k = sa.clear;
    if (k.valid && sa_batch_touches(k.fbp, k.fbw, k.fpsm, k.zbp, k.zpsm, k.rows)) {
        sa.clear.valid = false;
        if (sa.log) {
            fprintf(stderr, "[sa] the GS's clear drawn over: prim %#x frame %#llx zbuf %#llx test %#llx tex0 %#llx n %zu\n", prim,
                    (unsigned long long)frame, (unsigned long long)zbuf, (unsigned long long)test, (unsigned long long)tex0,
                    gs->verts.size());
        }
    }
    SaHeld *h = sa_held_of(fbp, fbw, fpsm, false);
    if (!h || !h->valid || (sa.bound && sa.fbp == fbp && sa.fbw == fbw && sa.fpsm == fpsm)) {
        return;
    }
    // one colour over every pixel of the frame: its RGB written whole, no DATE, Z not tested, the alpha test
    // passing, or failing into the frame all the same (AFAIL FB_ONLY, RGB_ONLY: the game's fills test NEVER so),
    // the sprites over the whole of the world's rows
    if ((uint32_t(frame >> 32) & 0xFFFFFF) || date || ztst != 1 || !from_origin || !sa_sprites_cover(fbw, h->rows, &rgba)) {
        return;
    }
    if (ate) {
        uint32_t afail = bits(test, 12, 2);
        if (afail != 1 && afail != 3) {
            int as = int(rgba >> 24), ref = int(bits(test, 4, 8));
            bool pass = false;
            switch (bits(test, 1, 3)) {
            case 2: pass = as < ref; break;
            case 3: pass = as <= ref; break;
            case 4: pass = as == ref; break;
            case 5: pass = as >= ref; break;
            case 6: pass = as > ref; break;
            case 7: pass = as != ref; break;
            default: break;
            }
            if (tme || !pass) {
                return; // (each texel its own, or nothing written)
            }
        }
    }
    uint32_t op[4] = {0, rgba, 0, 0};
    if (!tme) {
        if (!abe) {
            op[0] = 3; // (set)
        } else {
            uint32_t a = bits(alpha, 0, 2), b = bits(alpha, 2, 2), c = bits(alpha, 4, 2), d = bits(alpha, 6, 2);
            if (c == 1 || a == 3 || b == 3 || d == 3 || bits(gs->reg[R_PABE], 0, 1)) {
                return; // (by the frame's own alpha, or each pixel its own: not one colour over all)
            }
            op[0] = 2 | (bits(gs->reg[R_COLCLAMP], 0, 1) << 8);
            op[2] = a | b << 2 | c << 4 | d << 6;
            op[3] = bits(alpha, 32, 8);
        }
    } else {
        // the frame itself read a texel a pixel (FST, nearest), modulated by the colour, no blending: the brightness,
        // the tint
        uint32_t tbp = bits(tex0, 0, 14), tbw = bits(tex0, 14, 6), tpsm = bits(tex0, 20, 6), tfx = bits(tex0, 35, 2);
        uint32_t ba = bits(alpha, 0, 2), bb = bits(alpha, 2, 2), bd = bits(alpha, 6, 2);
        if (tbp != fbp || tbw != fbw || tpsm != fpsm || tfx != 0 || !fst || bits(tex1, 0, 1) || (abe && !(ba == bb && bd == 0))) {
            return;
        }
        // (each pixel its own texel: a corner's texel at its place, or less than one on where the sprite's edge
        // is half a pixel short of its texels', as the game's 447.5 rows over 448 texels)
        for (const Vtx &v : gs->verts) {
            float ds = v.s - v.x, dt = v.t - v.y;
            if (ds < -1.0f / 64.0f || dt < -1.0f / 64.0f || ds > 0.5f || dt > 0.5f || (v.x <= 0.0f && fabsf(ds) > 1.0f / 64.0f) ||
                (v.y <= 0.0f && fabsf(dt) > 1.0f / 64.0f)) {
                return;
            }
        }
        op[0] = 1;
    }
    if (h->count >= 8) {
        h->valid = false; // (more than the composite takes: the GS's picture, as it is)
        return;
    }
    memcpy(h->ops[h->count++], op, sizeof(op));
}

// (scanout_to_image, a whole picture) Whether the picture of the shown buffer is the composite's (a frame the scene
// drew at a scale of its own, kept and still good), and its size
const SaHeld *sa_composite_of(const Shown &s) {
    if (!sa.on || !s.t) {
        return nullptr;
    }
    const SaHeld *h = sa_held_of(s.fbp, s.fbw, s.psm, false);
    if (!h || !h->valid || h->t != s.t || sa_vblanks - h->vblank > 3 || !h->lo || s.t->image->get_width() != h->lo->get_width() ||
        s.t->image->get_height() != h->lo->get_height() || s.t->image->get_format() != h->lo->get_format() ||
        (h->sx == s.sx && h->sy == s.sy) || h->gsx != s.sx || h->gsy != s.sy) {
        return nullptr;
    }
    // (the picture's scale: the scene's, or with --upscaler the GS's)
    uint32_t ox = sa_held_upscaled(*h) ? h->gsx : h->sx, oy = sa_held_upscaled(*h) ? h->gsy : h->sy;
    if (size_t(s.w) * ox * s.h * oy > PORT_GS_FRAME_PIXELS || (s.dbx + s.w) * h->sx > h->hi->get_width() ||
        (s.dby + s.h) * h->sy > h->hi->get_height()) {
        return nullptr;
    }
    return h;
}
bool sa_composite_size(const Shown &s, uint32_t *iw, uint32_t *ih) {
    const SaHeld *h = sa_composite_of(s);
    if (!h) {
        return false;
    }
    if (iw) {
        bool up = sa_held_upscaled(*h);
        *iw = s.w * (up ? h->gsx : h->sx);
        *ih = s.h * (up ? h->gsy : h->sy);
    }
    return true;
}

// (sa_upscale_held, --upscaler fsr3 or dlss) Which temporal upscaler takes the held picture (sa_upscaler's 2 or 3; 0
// none): its depth kept, not in a recording (the composite never is). Where DLSS cannot be had, FSR from then on (said
// by upscale_dlss.cpp); where neither can, bilinear (--upscaler bilinear: the scene no longer jittered): its colours
// 16-bit (--native-colour high: FFX has no 16-bit UNORM; one path for both), an output a shader cannot write, FSR's
// DLL not there (said by upscale_ffx.cpp), or its calls failing (by gsn_upscale.inc)
int sa_temporal_takes(const SaHeld &h) {
    if (sa_upscaler < 2 || !h.depth || recording_ops->on) {
        return 0;
    }
    if (sa_upscaler == 3 && dlss_failed.load(std::memory_order_relaxed)) {
        fprintf(stderr, "[gsn] --upscaler dlss: FSR instead\n");
        sa_upscaler = 2;
    }
    static int storage = -1;
    if (storage < 0) {
        storage = gs->device->image_format_is_supported(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_FEATURE_2_STORAGE_IMAGE_BIT);
    }
    const char *why = nullptr;
    if (h.hi->get_format() != VK_FORMAT_R8G8B8A8_UNORM) {
        why = "the scene's colours are 16-bit (--native-colour high), which FSR does not take";
    } else if (storage <= 0) {
        why = "the GPU cannot write the picture from a shader";
    } else if (sa_upscaler == 2 && (upscale_failed.load(std::memory_order_relaxed) || !ffx_upscale_load())) {
        why = "";
    }
    if (why) {
        if (*why) {
            fprintf(stderr, "[gsn] --upscaler: %s: bilinear instead\n", why);
        }
        sa_upscaler = 1;
        return 0;
    }
    return sa_upscaler;
}

// (sa_composite, --upscaler) The held scene brought up to the GS's scale: 1, bilinear (a blit); 2, FSR 3.1, or 3,
// DLSS, temporal, with the picture's depth, motion and jitter, on the back end's thread (OP_UPSCALE, gsn_upscale.inc).
// Their history is of the pictures upscaled in the order shown: after a while without (a menu's pictures, say: more
// than 4 pictures), it begins again (reset), and the motion is from nothing
const Image &sa_upscale_held(SaHeld &h) {
    uint32_t W = h.hi->get_width(), H = h.hi->get_height();
    uint32_t uw = W / h.sx * h.gsx, uh = H / h.sy * h.gsy;
    int temporal = sa_temporal_takes(h);
    if (!h.up || h.up->get_width() != uw || h.up->get_height() != uh || h.up->get_format() != h.hi->get_format() ||
        (temporal && !(h.up->get_create_info().usage & VK_IMAGE_USAGE_STORAGE_BIT))) {
        h.up = sa_image(uw, uh, h.hi->get_format(), 1, temporal ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    }
    SaUpscaleLast &last = sa_upscale_last;
    auto now = std::chrono::steady_clock::now();
    bool reset = !last.valid || gsn_stats.pictures - last.picture > 4;
    if (reset) {
        sa_motion_last.valid = false;
    }
    float frame_ms = reset ? 0.0f : float(std::chrono::duration<double, std::milli>(now - last.at).count());
    last.valid = true;
    last.picture = gsn_stats.pictures;
    last.at = now;
    bool show = false;
    if (sa_motion_wanted() && h.depth) {
        reset = reset || !sa_motion_last.valid;
        sa_motion_pass(h); // (the picture's motion from the last one shown: the temporal upscalers', or to look at)
        show = sa_motion_show();
    }
    end_pass();
    if (temporal && !show) {
        const float *jitter = h.between ? sa_cam.picture_jitter : h.jitter;
        ArgUpscale a = {uint32_t(temporal), h.hi.get(), h.fdepth.get(), h.motion.get(), h.up.get(),
                        {jitter[0], jitter[1]}, std::min(std::max(frame_ms, 1.0f), 100.0f), h.near_plane, h.fov_y,
                        reset};
        Cmd &c = cmd();
        c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        c.upscale(a);
        c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                  VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
        sa_stats.upscales_temporal++;
    } else {
        transfer_barrier();
        cmd().blit_image(*h.up, show ? *h.shown : *h.hi, {0, 0, 0}, {int32_t(uw), int32_t(uh), 1}, {0, 0, 0},
                         {int32_t(W), int32_t(H), 1}, VK_FILTER_LINEAR);
        transfer_barrier();
    }
    sa_stats.upscales++;
    return *h.up;
}

// The composite's picture into dst (as large as sa_composite_size says): shaders/sa_composite.frag
void sa_composite(const Image &dst, const Shown &s) {
    const SaHeld *h = sa_composite_of(s);
    if (!sa_program_composite) {
        ResourceLayout vert = {};
        ResourceLayout frag = {};
        frag.output_mask = 1;
        frag.push_constant_size = 32;
        frag.sets[0].sampled_image_mask = 7;
        frag.sets[0].fp_mask = 7;
        frag.sets[0].uniform_buffer_mask = 1u << 3;
        for (int b = 0; b < 4; b++) {
            frag.sets[0].meta[b].array_size = 1;
        }
        sa_program_composite = gs->device->request_program(
            gs->device->request_shader(gsn_spv_msaa_vert, sizeof(gsn_spv_msaa_vert), &vert),
            gs->device->request_shader(gsn_spv_sa_composite_frag, sizeof(gsn_spv_sa_composite_frag), &frag));
    }
    msaa_settle(*s.t);
    end_pass();
    // (--upscaler: the scene at the GS's scale, the picture too)
    bool up = sa_held_upscaled(*h);
    const Image &scene = up ? sa_upscale_held(*sa_held_of(s.fbp, s.fbw, s.psm, false)) : *h->hi;
    uint32_t ox = up ? h->gsx : h->sx, oy = up ? h->gsy : h->sy;
    RenderPassInfo rp;
    rp.num_color_attachments = 1;
    rp.color_attachments[0] = &dst.get_view();
    rp.store_attachments = 1;
    Cmd &c = cmd();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    c.begin_render_pass(rp);
    c.set_opaque_state();
    c.set_program(sa_program_composite);
    c.set_primitive_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    c.set_cull_mode(VK_CULL_MODE_NONE);
    c.set_depth_test(false, false);
    c.set_texture(0, 0, s.t->image->get_view(), StockSampler::NearestClamp);
    c.set_texture(0, 1, h->lo->get_view(), StockSampler::NearestClamp);
    c.set_texture(0, 2, scene.get_view(), StockSampler::NearestClamp);
    uint32_t *chain = c.allocate_typed_constant_data<uint32_t>(0, 3, 32);
    memset(chain, 0, 32 * sizeof(uint32_t));
    memcpy(chain, h->ops, sizeof(uint32_t) * 4 * size_t(h->count));
    struct {
        float to_gs[2];
        int32_t gs_origin[2], hi_origin[2];
        int32_t count;
        float tol;
    } push = {{float(h->gsx) / float(ox), float(h->gsy) / float(oy)},
              {int32_t(s.dbx * h->gsx), int32_t(s.dby * h->gsy)},
              {int32_t(s.dbx * ox), int32_t(s.dby * oy)},
              h->count,
              2.0f};
    c.push_constants(&push, 0, sizeof(push));
    c.draw(3);
    c.end_render_pass();
    c.barrier(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
              VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    sa_stats.composites++;
    sa_stats.composite_ops += uint64_t(h->count);
}
