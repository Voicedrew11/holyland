// ---- Standalone (P8, T3): render targets by name -----------------------------------------------------
// (docs/port/NATIVE_DRAW_P8.md, "T3".) Included at the end of gsn_standalone.inc, inside gsn.cpp's
// namespace. The table's target side: the render targets the game draws into besides the frame (the
// reflection, mode 2's 256 x 256 at 0x2BC0; the scrolling textures' 128 x 128 targets; the refracting
// entity's frame copy at 0x3540; ...) are the scene's images, named by the buffer they are (FRAME's base,
// width and format), and own their pages in the table: a texture on those pages, drawn into after any
// upload of them, is read from the scene's image, not from the GS's targets or VRAM.
//
// What a named target holds comes from whoever drew into it last:
//   - the scene (sa_draw_batch, with the target bound as the scene's frame: sat_acquire), for the 3D the
//     scene draws there: the reflection's map records, objects, entities, items and effects, with a depth
//     of the scene's own (D32F, reversed, unrounded as the frame's). Its clear (GsDraw_ClearFrame's sprite)
//     is the scene's too (sat_flush): the colour and the depth cleared (the depth to the far plane: the PS2
//     tested the reflection against the main Z buffer's last frame, read 256 wide, garbage that can only
//     make holes). The GS's target is given the picture (sat_give_back) only when the GS wants it: a batch
//     of the GS's that reads or draws there, a transfer over it, a read-back or picture (sat_sync);
//   - the GS, for what it draws there (the scrolling textures' sprites, the frame copied by a sprite): the
//     scene's image is made again from the GS's target (sat_refresh) when the scene next reads it.
// So the scene can draw a target the GS never sees (with the water in the scene too, the reflection never
// goes back to the GS), and a target the scene never draws is the GS's picture as before.
//
// A local copy (BITBLT, TRXDIR 2) whose source is all the table's uploads (MapScroll_UpdateCopy's in-place
// scroll through 0x3C40: the texture copied to the spare buffer and back, shifted) is done in the table as
// well (sat_copied): the source's uploads written into the scratch VRAM, the copy made there as the GS
// makes it (gsn_vram_copy), and the destination's bytes kept as an upload of their own. The scrolled
// texture is then the table's like any other, exact, and found again when the scroll comes round.
//
// KF4_SA_NO_NAMED=1 (tests): none of this (the GS's targets and VRAM, as before T3).

struct SatTarget {
    uint32_t id = 0;
    uint32_t bp = 0, bw = 0, psm = 0; // the GS buffer it is (FRAME's base in blocks, width in 64s, format)
    uint32_t width = 0, height = 0;   // in GS pixels
    uint32_t sx = 1, sy = 1;          // its images' scale (the GS's targets')
    ImageHandle color, depth, copy;   // the scene's (depth: only once the scene draws into it)
    bool scene_newer = false;         // the scene drew into it since the GS's target had its picture
    bool gs_newer = false;            // the GS drew into it (or a transfer wrote it) since the image was made from it
    bool depth_clear = false;         // its depth cleared since it was made
    uint32_t stencil_used = 0;        // (pf/skin) the largest number in its stencil since it was cleared (sa.stencil_gen's)
    bool z_dirty = false;             // its depth drawn into (or cleared) since the GS's Z buffer had it
    uint32_t zrows0 = UINT32_MAX, zrows1 = 0; // (those rows of it)
    uint32_t zbp = 0, zpsm = 0;       // the Z buffer the scene's draws there name (ZBUF's)
    uint32_t rows0 = UINT32_MAX, rows1 = 0; // the rows the scene drew since the GS had it (GS pixels)
    Target fake;                      // (sa.frame_target while it is bound: its size and scale; no image)
};

struct SatStats {
    uint64_t made, binds, clears, refreshes, give_backs, textures, copies, copy_bytes, copies_not, fills, overwritten;
};
SatStats sat_stats;

struct Sat {
    bool off = false; // (KF4_SA_NO_NAMED)
    std::vector<std::unique_ptr<SatTarget>> list;
    uint32_t page_named[512] = {}; // the named target that last drew into each page (0: none)
    uint32_t next_id = 1;
    // the bound one (as the scene's frame), and the frame's images while it is
    SatTarget *bound = nullptr;
    ImageHandle frame_color, frame_depth, frame_copy;
    uint32_t frame_samples = 0;
    uint32_t frame_sx = 1, frame_sy = 1; // (the scene's scale, T2's --scene-scale: a named target is at the GS's)
    bool writing = false; // (sat_give_back's mark_dirty: not the GS's drawing)
    bool fill = false;    // (the batch sa_draw_batch draws is a fill: sat_fill)
} snt; // (the named targets: not the table's `sat`, gsn_standalone.inc)

// Formats a named target can be: the GS's colour targets
bool sat_format(uint32_t psm) {
    return psm == GSN_PSMCT32 || psm == GSN_PSMCT24 || psm == GSN_PSMCT16 || psm == GSN_PSMCT16S;
}

// A buffer drawn into that is a named target: narrower than the frame (sa_draw_batch's rule for the frame:
// 512 pixels or more across), in a colour format
// (sa_draw_batch, upscaling) The picture's jitter goes to the scene's frame only, not to a named target bound as it
// (the reflection, a scrolling texture): those are textures the frame reads, not the picture an upscaler takes
bool sa_jitter_draw() {
    return sa_jitter_on() && !snt.bound;
}

bool sat_narrow(uint32_t bw, uint32_t psm) {
    return !snt.off && sat_format(psm) && bw > 0 && bw * 64 < 512;
}

SatTarget *sat_find(uint32_t bp, uint32_t bw, uint32_t psm) {
    for (auto &n : snt.list) {
        if (n->bp == bp && n->bw == bw && psm_layout(n->psm) == psm_layout(psm)) {
            return n.get();
        }
    }
    return nullptr;
}

SatTarget *sat_get(uint32_t bp, uint32_t bw, uint32_t psm) {
    SatTarget *n = sat_find(bp, bw, psm);
    if (n) {
        n->psm = psm;
        return n;
    }
    snt.list.push_back(std::make_unique<SatTarget>());
    n = snt.list.back().get();
    n->id = snt.next_id++;
    n->bp = bp;
    n->bw = bw;
    n->psm = psm;
    n->width = bw * 64;
    sat_stats.made++;
    return n;
}

// Its pages, rows y0..y1-1, are its own in the table (drawn into after any upload of them)
void sat_own_rows(SatTarget &n, uint32_t y0, uint32_t y1) {
    if (y1 <= y0) {
        return;
    }
    uint32_t lo, count;
    buffer_pages(n.bp, n.bw, n.psm, n.width, y1, &lo, &count);
    uint32_t across = std::max(n.bw * 64 / page_width(n.psm), 1u);
    uint32_t first = std::min(y0 / page_height(n.psm) * across, count);
    sa_pages_drawn(lo + first, count - first, n.psm); // (T4's table: in the nibbles its format has)
    for (uint32_t k = first; k < count; k++) {
        snt.page_named[(lo + k) % vram_pages] = n.id;
    }
}

// (sa_drawn: mark_dirty) A target of the GS's drawn into: its pages are a named target's (marked newer in
// the GS) or no named target's
void sat_drawn(const Target &t, uint32_t y0, uint32_t y1) {
    if (snt.off) {
        return;
    }
    SatTarget *n = !t.depth && sat_narrow(t.bw, t.psm) ? sat_get(t.bp, t.bw, t.psm) : nullptr;
    uint32_t lo, count;
    buffer_pages(t.bp, t.bw, t.psm, t.width, std::min(y1, t.height), &lo, &count);
    uint32_t across = std::max(std::max(t.bw, 1u) * 64 / page_width(t.psm), 1u);
    for (uint32_t k = std::min(y0 / page_height(t.psm) * across, count); k < count; k++) {
        snt.page_named[(lo + k) % vram_pages] = n ? n->id : 0;
    }
    if (n && !snt.writing) {
        if (sa.log == 3) {
            fprintf(stderr, "[sa] the GS drew into named target %#x rows %u-%u\n", n->bp, y0, y1);
        }
        n->gs_newer = true;
        n->height = std::max(n->height, std::min(t.height, 2048u));
    }
}

// The images made (again) as big as w x h GS pixels at sx, sy (what they had kept where they overlap)
void sat_images(SatTarget &n, uint32_t w, uint32_t h, uint32_t sx, uint32_t sy, VkFormat format, bool depth) {
    bool remake = !n.color || n.color->get_width() != w * sx || n.color->get_height() != h * sy ||
                  n.color->get_format() != format || n.sx != sx || n.sy != sy;
    if (remake) {
        ImageHandle old = n.color;
        n.color = sa_image(w * sx, h * sy, format, 1);
        if (old && n.sx == sx && n.sy == sy && old->get_format() == format) {
            {
                Why why(GSN_WHY_COPY);
                end_pass();
            }
            transfer_barrier();
            uint32_t cw = std::min(old->get_width(), n.color->get_width()), ch = std::min(old->get_height(), n.color->get_height());
            cmd().copy_image(*n.color, *old, {0, 0, 0}, {0, 0, 0}, {cw, ch, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                             {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
            transfer_barrier();
        } else {
            n.gs_newer = true; // (nothing kept: the GS's, if it has it)
        }
        n.depth.reset();
        n.copy.reset();
        n.depth_clear = false;
        n.sx = sx;
        n.sy = sy;
    }
    n.width = w;
    n.height = h;
    if (depth && (!n.depth || n.depth->get_format() != sa.depth_format)) {
        n.depth = sa_image(w * sx, h * sy, sa.depth_format, 1);
        n.depth_clear = false;
    }
    if (depth && !n.depth_clear) {
        // (the far plane: reversed Z)
        {
            Why why(GSN_WHY_COPY);
            end_pass();
        }
        transfer_barrier();
        VkClearValue clear = {};
        cmd().clear_image(*n.depth, clear);
        transfer_barrier();
        n.depth_clear = true;
        n.stencil_used = 0;
    }
    n.fake.bp = n.bp;
    n.fake.bw = n.bw;
    n.fake.psm = n.psm;
    n.fake.width = n.width;
    n.fake.height = n.height;
    n.fake.sx = n.sx;
    n.fake.sy = n.sy;
    n.fake.depth = false;
}

// The scene's images made from the GS's targets, if the GS drew into it since: its colour, and the Z buffer the
// scene's draws there name (sa_acquire's copies). False: the GS has no target there (its picture is in VRAM
// only): the texture is the GS's
bool sat_refresh(SatTarget &n) {
    if (!n.gs_newer && n.color) {
        return true;
    }
    Target *t = find_target(n.bp, n.bw, n.psm);
    if (!t || t->depth || !t->image || n.scene_newer) {
        return false;
    }
    if (snt.bound == &n) {
        sa_release();
    }
    bool depth = bool(n.depth);
    sat_images(n, t->width, t->height, t->sx, t->sy, t->image->get_format(), depth);
    // (the GS's Z buffer, when it has the newer depth: not when the scene's depth was not given back)
    Target *z = depth && n.zpsm && !n.z_dirty ? find_target(n.zbp, n.bw, n.zpsm) : nullptr;
    if (z && (!z->depth || z->sx != t->sx || z->sy != t->sy)) {
        z = nullptr;
    }
    {
        Why why(GSN_WHY_COPY);
        msaa_settle(*t);
        if (z) {
            msaa_settle(*z);
        }
        end_pass();
    }
    gpu_mark(GPU_OTHER);
    transfer_barrier();
    uint32_t w = std::min(t->image->get_width(), n.color->get_width()), h = std::min(t->image->get_height(), n.color->get_height());
    cmd().copy_image(*n.color, *t->image, {0, 0, 0}, {0, 0, 0}, {w, h, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
    transfer_barrier();
    gsn_stats.copy_bytes += uint64_t(w) * h * (wide(*n.color) ? 8 : 4);
    if (z) {
        // (as sa_acquire takes the frame's Z buffer: its depth as it is, the stencil cleared)
        uint32_t zh = std::min(z->image->get_height(), n.depth->get_height());
        if (sa.stencil) {
            float zpush[2] = {-1.0f, 1.0e30f};
            sa_copy_pass(*n.depth, *z->image, sa.program_depth, true, 0, zh, zpush, sizeof(zpush), true);
            n.stencil_used = 0;
        } else {
            cmd().copy_image(*n.depth, *z->image, {0, 0, 0}, {0, 0, 0}, {std::min(w, z->image->get_width()), zh, 1},
                             {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1}, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1});
            transfer_barrier();
        }
        gsn_stats.copy_bytes += uint64_t(w) * zh * 4;
    }
    gpu_mark(GPU_COPY);
    n.gs_newer = false;
    sat_stats.refreshes++;
    return true;
}

// The GS's targets given the scene's picture (the rows the scene drew), before the GS touches them: the colour,
// and the depth into the Z buffer the scene's draws there named (as sa_release gives the frame's back), so what
// the GS draws there after (the PS2 path's map strips the scene does not take) is tested against it
void sat_give_back(SatTarget &n, const char *why, bool with_depth) {
    bool colour = n.scene_newer, depth = with_depth && n.z_dirty && n.depth && n.zpsm;
    if (!colour && !depth) {
        return;
    }
    if (sa.log) {
        fprintf(stderr, "[sa] named target %#x given back to the GS%s: %s\n", n.bp, depth ? " (and its depth)" : "", why);
    }
    if (snt.bound == &n) {
        sa_release();
        colour = n.scene_newer;
        depth = with_depth && n.z_dirty && n.depth && n.zpsm;
    }
    uint32_t y0 = std::min(colour ? n.rows0 : UINT32_MAX, depth ? n.zrows0 : UINT32_MAX);
    uint32_t y1 = std::min(std::max(colour ? n.rows1 : 0u, depth ? n.zrows1 : 0u), n.height);
    if (colour) {
        n.scene_newer = false;
        n.rows0 = UINT32_MAX;
        n.rows1 = 0;
    }
    if (depth) {
        n.z_dirty = false;
        n.zrows0 = UINT32_MAX;
        n.zrows1 = 0;
    }
    if (y1 <= y0 || !n.color) {
        return;
    }
    Target *t = colour ? get_target(n.bp, n.bw, n.psm, n.width, y1) : nullptr;
    Target *z = depth ? get_target(n.zbp, n.bw, n.zpsm, n.width, y1) : nullptr;
    if (z && colour && find_target(n.bp, n.bw, n.psm) != t) {
        t = get_target(n.bp, n.bw, n.psm, n.width, y1); // (made again, as flush_batch looks its two up again)
    }
    if ((colour && (!t || t->depth || t->sx != n.sx || t->sy != n.sy || t->image->get_format() != n.color->get_format())) ||
        (depth && (!z || !z->depth || z->sx != n.sx || z->sy != n.sy))) {
        static int told;
        if (!told++) {
            fprintf(stderr, "[gsn] standalone: a named target (%#x) could not be given back to the GS\n", n.bp);
        }
        return;
    }
    {
        Why why_(GSN_WHY_COPY);
        if (t) {
            msaa_settle(*t);
        }
        if (z) {
            msaa_settle(*z);
        }
        end_pass();
    }
    gpu_mark(GPU_OTHER);
    transfer_barrier();
    uint32_t r0 = y0 * n.sy, r1 = std::min(y1 * n.sy, n.color->get_height());
    if (t && std::min(r1, t->image->get_height()) > r0) {
        uint32_t w = std::min(t->image->get_width(), n.color->get_width()), tr1 = std::min(r1, t->image->get_height());
        cmd().copy_image(*t->image, *n.color, {0, int32_t(r0), 0}, {0, int32_t(r0), 0}, {w, tr1 - r0, 1},
                         {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1});
        transfer_barrier();
        gsn_stats.copy_bytes += uint64_t(w) * (tr1 - r0) * (wide(*n.color) ? 8 : 4);
    }
    if (z && std::min(r1, z->image->get_height()) > r0) {
        // (the depth as the GS keeps it: with its fraction under --native-layer-z tolerant, else whole Z)
        float zpush[2] = {float(depth_scale(n.zpsm)) * (layer_z_mode == GSN_LAYER_Z_TOLERANT ? -1.0f : 1.0f),
                          float(depth_max(n.zpsm))};
        uint32_t zr1 = std::min(r1, z->image->get_height());
        sa_copy_pass(*z->image, *n.depth, sa.program_depth, true, r0, zr1, zpush, sizeof(zpush));
        gsn_stats.copy_bytes += uint64_t(n.color->get_width()) * (zr1 - r0) * 4;
    }
    gpu_mark(GPU_COPY);
    snt.writing = true;
    if (t) {
        msaa_image_written(*t);
        mark_dirty(*t, y0, y1);
    }
    if (z) {
        msaa_image_written(*z);
        mark_dirty(*z, y0, y1);
    }
    snt.writing = false;
    sat_stats.give_backs++;
}

// (sa_draw_batch) Whether a batch's frame is a named target the scene draws into
bool sat_scene_frame(uint32_t fbw, uint32_t fpsm) {
    return sa.on && sat_narrow(fbw, fpsm);
}

// (sa_acquire, for a named target's frame) The named target bound as the scene's frame: its images in place
// of the frame's (which the scene gives back first), its own depth. The Z buffer the batch names is not
// read: the scene's depth for it is its own.
bool sat_acquire(uint32_t fbp, uint32_t fbw, uint32_t fpsm, uint32_t zbp, uint32_t zpsm, uint32_t need_h) {
    SatTarget *n = sat_get(fbp, fbw, fpsm);
    if (snt.bound == n && sa.bound && need_h <= n->height && n->zbp == zbp && n->zpsm == zpsm) {
        return true;
    }
    sa_release();
    sa_programs();
    if (n->zbp != zbp || n->zpsm != zpsm) {
        n->zbp = zbp;
        n->zpsm = zpsm;
    }
    if (n->gs_newer) {
        sat_refresh(*n); // (what the GS drew there is what the scene draws over)
    }
    uint32_t h = std::max(n->height, need_h);
    Target *t = find_target(fbp, fbw, fpsm);
    if (t && !t->depth) {
        h = std::max(h, t->height);
    }
    sat_images(*n, n->width, h, target_sx, target_sy, colour_format(), true);
    snt.frame_color = sa.color;
    snt.frame_depth = sa.depth;
    snt.frame_copy = sa.copy;
    snt.frame_samples = sa.samples;
    snt.frame_sx = sa.sx;
    snt.frame_sy = sa.sy;
    sa.color = n->color;
    sa.depth = n->depth;
    sa.copy = n->copy;
    sa.samples = 1;
    sa.sx = n->sx;
    sa.sy = n->sy;
    sa.bound = true;
    sa.fbp = fbp;
    sa.fbw = fbw;
    sa.fpsm = fpsm;
    sa.zbp = zbp;
    sa.zpsm = zpsm;
    sa.frame_target = &n->fake;
    sa.z_target = &n->fake;
    sa.rows0 = UINT32_MAX;
    sa.rows1 = 0;
    sa.z_written = false;
    sa.stencil_gen = 0;
    sa.last_shadow = false;
    snt.bound = n;
    sat_stats.binds++;
    return true;
}

// (sa_release, at its top) A named target bound: let go (nothing copied: its picture is the scene's), the
// frame's images back. False: none bound.
bool sat_bound() {
    return snt.bound != nullptr;
}

// (pf/skin, sa_draw_batch) A number for a skinned group's marks in the bound named target's stencil that none there has:
// its turns before this one (since its stencil was cleared) may have used some (a turn starts at 0 again). False: none.
bool sat_stencil_number(uint32_t *gen) {
    uint32_t base = std::max(sa.stencil_gen, snt.bound->stencil_used);
    if (base >= 200) {
        return false;
    }
    sa.stencil_gen = base + 1;
    *gen = sa.stencil_gen;
    return true;
}

bool sat_release() {
    if (!snt.bound) {
        return false;
    }
    {
        Why why(GSN_WHY_COPY);
        end_pass();
    }
    SatTarget &n = *snt.bound;
    snt.bound = nullptr;
    n.stencil_used = std::max(n.stencil_used, sa.stencil_gen);
    if (sa.rows1 > sa.rows0) {
        n.scene_newer = true;
        n.gs_newer = false;
        if (sa.z_written) {
            n.z_dirty = true;
            n.zrows0 = std::min(n.zrows0, sa.rows0);
            n.zrows1 = std::max(n.zrows1, std::min(sa.rows1, n.height));
        }
        n.rows0 = std::min(n.rows0, sa.rows0);
        n.rows1 = std::max(n.rows1, std::min(sa.rows1, n.height));
        sat_own_rows(n, sa.rows0, std::min(sa.rows1, n.height));
    }
    n.copy = sa.copy;
    sa.color = snt.frame_color;
    sa.depth = snt.frame_depth;
    sa.copy = snt.frame_copy;
    sa.samples = snt.frame_samples;
    sa.sx = snt.frame_sx;
    sa.sy = snt.frame_sy;
    snt.frame_color.reset();
    snt.frame_depth.reset();
    snt.frame_copy.reset();
    sa.bound = false;
    sa.frame_target = sa.z_target = nullptr;
    return true;
}

// The named targets whose pages a rectangle of a buffer reaches
template <class F> void sat_touched(uint32_t bp, uint32_t bw, uint32_t psm, uint32_t x1, uint32_t y1, F &&f) {
    uint32_t lo, count;
    buffer_pages(bp, bw, psm, x1, y1, &lo, &count);
    for (auto &n : snt.list) {
        uint32_t nlo, ncount;
        buffer_pages(n->bp, n->bw, n->psm, n->width, std::max(n->height, 1u), &nlo, &ncount);
        if (pages_overlap(lo, count, nlo, ncount)) {
            f(*n);
        }
    }
}

bool sat_any_scene_newer() {
    for (auto &n : snt.list) {
        if (n->scene_newer || n->z_dirty || snt.bound == n.get()) {
            return true;
        }
    }
    return false;
}

// (sa_flush, a batch the GS draws) What it reads or draws into of a named target the scene drew is the GS's
// first: its FRAME's pages, its texture's, and (when it uses Z) its Z buffer's
void sat_gs_batch() {
    if (snt.off || !sat_any_scene_newer()) {
        return;
    }
    uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
    uint64_t frame = gs->reg[R_FRAME_1 + ctxt], zbuf = gs->reg[R_ZBUF_1 + ctxt], test = gs->reg[R_TEST_1 + ctxt];
    uint64_t scissor = gs->reg[R_SCISSOR_1 + ctxt], tex0 = gs->reg[R_TEX0_1 + ctxt];
    uint32_t fbw = bits(frame, 16, 6), h = bits(scissor, 48, 11) + 1;
    // (a batch all outside its scissor draws nothing: the PS2 path's map strips of the reflection the scene does
    // not take are, from where 009's save stands)
    if (!interp.active && !gs->verts.empty()) {
        float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
        for (const Vtx &v : gs->verts) {
            x0 = std::min(x0, v.x);
            y0 = std::min(y0, v.y);
            x1 = std::max(x1, v.x);
            y1 = std::max(y1, v.y);
        }
        if (x1 < float(bits(scissor, 0, 11)) - 1.0f || x0 > float(bits(scissor, 16, 11)) + 1.0f ||
            y1 < float(bits(scissor, 32, 11)) - 1.0f || y0 > float(h) + 1.0f) {
            return;
        }
    }
    auto give = [](SatTarget &n) {
        sat_give_back(n, "a batch of the GS's", true);
    };
    auto give_colour = [](SatTarget &n) {
        sat_give_back(n, "a batch of the GS's", false);
    };
    // (its frame: with the depth when it uses Z, as flush_batch tests what is there)
    bool uses_z = bits(test, 16, 1) && (bits(test, 17, 2) != 1 || !bits(zbuf, 32, 1));
    sat_touched(bits(frame, 0, 9) * 32, fbw, bits(frame, 24, 6), std::max(fbw, 1u) * 64, h, uses_z ? give : give_colour);
    if (uses_z) {
        sat_touched(bits(zbuf, 0, 9) * 32, fbw, bits(zbuf, 24, 4) | 0x30, std::max(fbw, 1u) * 64, h, give);
    }
    if ((prim >> 4) & 1) {
        sat_touched(bits(tex0, 0, 14), bits(tex0, 14, 6), bits(tex0, 20, 6), 1u << std::min(bits(tex0, 26, 4), 10u),
                    1u << std::min(bits(tex0, 30, 4), 10u), give_colour);
    }
}

// (sa_transfer, at its top) A transfer: the named targets it reads given to the GS first, and those it writes
// the GS's after it (an upload's pages are the table's, a copy's the GS's or the table's: sat_copied). True:
// a named target is bound (let go if the transfer touches it; the frame's turn is not the scene's then).
bool sat_transfer(uint32_t dir) {
    if (snt.off || !sa.on || snt.list.empty()) {
        return false;
    }
    bool was_bound = snt.bound != nullptr;
    uint64_t bitbltbuf = gs->reg[R_BITBLTBUF], trxpos = gs->reg[R_TRXPOS], trxreg = gs->reg[R_TRXREG];
    uint32_t w = bits(trxreg, 0, 12), h = bits(trxreg, 32, 12);
    auto touch = [&](SatTarget &n) {
        sat_give_back(n, "a transfer", true);
    };
    // (dir 0, 2: the pages the transfer writes whole, every bit: whole_pages)
    std::vector<uint32_t> whole;
    bool whole_known = false;
    auto written = [&](SatTarget &n) {
        // A transfer that writes every page of the scene's colour whole leaves nothing of it: the scene's picture
        // is not given back first (it would be dropped at once). The MP drain's and the water's copy of the frame
        // to 0x2BC0 covers the reflection's 256 x 256 there: given back each frame, the GS's target was made for it
        // from VRAM, which first took the copy of the frame before back from the GPU (a read-back a frame, 640 x
        // 448, at a reflecting pool)
        if (n.scene_newer && n.color) {
            if (!whole_known) {
                whole = whole_pages(bitbltbuf, trxpos, trxreg);
                whole_known = true;
            }
            uint32_t lo, count;
            buffer_pages(n.bp, n.bw, n.psm, n.width, n.height, &lo, &count);
            bool covered = !whole.empty();
            for (uint32_t k = 0; k < count && covered; k++) {
                covered = std::find(whole.begin(), whole.end(), (lo + k) % vram_pages) != whole.end();
            }
            if (covered) {
                if (snt.bound == &n) {
                    sa_release();
                }
                n.scene_newer = false;
                n.rows0 = UINT32_MAX;
                n.rows1 = 0;
                sat_stats.overwritten++;
            }
        }
        touch(n);
        n.gs_newer = true;
    };
    if (dir == 0 || dir == 2) {
        sat_touched(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), bits(bitbltbuf, 56, 6), bits(trxpos, 32, 11) + w,
                    bits(trxpos, 48, 11) + h, written);
    }
    if (dir == 1 || dir == 2) {
        sat_touched(bits(bitbltbuf, 0, 14), bits(bitbltbuf, 16, 6), bits(bitbltbuf, 24, 6), bits(trxpos, 0, 11) + w,
                    bits(trxpos, 16, 11) + h, touch);
    }
    if (dir > 2) {
        for (auto &n : snt.list) {
            touch(*n);
        }
    }
    return was_bound;
}

// (sa_sync) The GS wants its targets (a picture, a read-back, a save state ...): each named target the scene
// drew given back
void sat_sync() {
    if (snt.off) {
        return;
    }
    sat_release();
    for (auto &n : snt.list) {
        sat_give_back(*n, "the GS's targets wanted", false);
    }
}

// (sa_flush, a batch of the GS's) The clear of a named target the scene draws into (GsDraw_ClearFrame's
// sprite: black, no texture, no blending, Z neither tested nor written, over all of it): the scene's own,
// its depth cleared with it. True: done (the batch is gone).
bool sat_flush() {
    if (snt.off || !gs->batch_sprites || gs->verts.empty() || nd.active) {
        return false;
    }
    uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
    uint64_t frame = gs->reg[R_FRAME_1 + ctxt], zbuf = gs->reg[R_ZBUF_1 + ctxt], test = gs->reg[R_TEST_1 + ctxt];
    uint64_t scissor = gs->reg[R_SCISSOR_1 + ctxt];
    uint32_t fbp = bits(frame, 0, 9) * 32, fbw = bits(frame, 16, 6), fpsm = bits(frame, 24, 6);
    if (!sat_narrow(fbw, fpsm) || ((prim >> 4) & 1) || ((prim >> 6) & 1) || uint32_t(frame >> 32) != 0) {
        return false;
    }
    bool zwrite = bits(test, 16, 1) && !bits(zbuf, 32, 1);
    bool ztest = bits(test, 16, 1) && bits(test, 17, 2) != 1;
    bool atest = bits(test, 0, 1) && bits(test, 1, 3) != 1;
    if (zwrite || ztest || atest || bits(test, 14, 1)) {
        return false;
    }
    uint32_t sx0 = bits(scissor, 0, 11), sx1 = bits(scissor, 16, 11), sy0 = bits(scissor, 32, 11), sy1 = bits(scissor, 48, 11);
    SatTarget *n = sat_find(fbp, fbw, fpsm);
    if (!n || !n->depth || sx0 != 0 || sy0 != 0 || sx1 + 1 < n->width || sy1 + 1 < n->height) {
        return false; // (only one the scene has drawn into: one the GS draws stays the GS's)
    }
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (const Vtx &v : gs->verts) {
        if (v.rgba != 0) {
            return false;
        }
        x0 = std::min(x0, v.x);
        y0 = std::min(y0, v.y);
        x1 = std::max(x1, v.x);
        y1 = std::max(y1, v.y);
    }
    if (x0 > 0.0f || y0 > 0.0f || x1 < float(n->width) || y1 < float(n->height)) {
        return false;
    }
    if (n->scene_newer || snt.bound == n) {
        if (snt.bound == n) {
            sa_release();
        }
    }
    if (sa.bound && !snt.bound) {
        // (the frame's turn goes on: the clear touches none of it)
    }
    {
        Why why(GSN_WHY_COPY);
        end_pass();
    }
    transfer_barrier();
    VkClearValue clear = {};
    cmd().clear_image(*n->color, clear);
    cmd().clear_image(*n->depth, clear);
    transfer_barrier();
    n->depth_clear = true;
    n->stencil_used = 0;
    n->z_dirty = true;
    n->zrows0 = 0;
    n->zrows1 = n->height;
    n->scene_newer = true;
    n->gs_newer = false;
    n->rows0 = 0;
    n->rows1 = n->height;
    sat_own_rows(*n, 0, n->height);
    sat_stats.clears++;
    gs->verts.clear();
    gs->batch_sprites = false;
    return true;
}

// The reflection kept for the pictures between (sa_flush, in a picture between; KF4_SA_REFLECTION_KEEP). A named target the
// scene draws into (n->depth: the reflection; not a scrolling texture's, which the GS draws) is left as the frame drew it,
// its clear and every batch into it left out:
//   1 (the default): when the camera of the frame shown is the frame's, bit for bit (interp.still: frame_interp.c compares
//      the two frames' view fields, GsnInterpFrame's still). Every camera between the two is then the frame's, and the
//      reflection's draws move with the camera only (gsn_native_draw.inc, nd_model_flags; frame_interp.c pairs no run of
//      the reflection with its model), so the frame's reflection is the picture's exactly. (Drawn again, it came out of
//      the camera rebuilt by camera_between, a rounding off the frame's: a last bit of depth that could turn a tie, 11
//      pixels of a picture once, where the kept one is the frame's to the bit.) The pond standing: about a third of the
//      GPU's work a picture left out (NATIVE_DRAW_P8.md, "The pond at 240");
//   0: drawn in every picture between;
//   2: whatever the camera does: the reflection then moves 30 times a second, the rest of the picture at the frame rate.
bool sat_kept(uint32_t bp, uint32_t bw, uint32_t psm) {
    int keep = reflection_keep();
    if (snt.off || !keep || !sat_narrow(bw, psm) || (keep == 1 && !interp.still)) {
        return false;
    }
    SatTarget *n = sat_find(bp, bw, psm);
    return n && n->depth && !n->gs_newer;
}

// (sa_texture) A texture on a named target's pages, all drawn into by it since their uploads: read from its
// image (as flush_batch reads a render target: SRC_TARGET_*, at the offset of its first page). False: not one.
bool sat_texture(SaTexture &st, uint32_t tbp, uint32_t tbw, uint32_t tpsm, uint32_t cols, uint32_t rows, uint32_t levels) {
    if (snt.off || levels > 1 || !sat_format(tpsm)) {
        return false;
    }
    for (auto &np : snt.list) {
        SatTarget &n = *np;
        if (n.bw != tbw || tbp < n.bp || psm_layout(tpsm) != psm_layout(n.psm) || (tbp - n.bp) % 32 != 0) {
            continue;
        }
        uint32_t page = (tbp - n.bp) / 32, across = std::max(n.bw, 1u);
        uint32_t x0 = (page % across) * page_width(n.psm), y0 = (page / across) * page_height(n.psm);
        if (x0 + cols > n.width || y0 + rows > n.height) {
            continue;
        }
        // (every block the texture reads: the named target's page, drawn into after its upload)
        bool own = true;
        sa_blocks_of(tbp, tbw, tpsm, 0, 0, cols, rows, [&](uint32_t block, const SaCover &need) {
            uint32_t p = block / 32;
            // (T4's table: drawn since every upload of the nibbles it reads, in each part of the word they are)
            uint64_t parts = need.folded(), drawn = UINT64_MAX, newest = 0;
            if (parts & SA_LO) {
                drawn = std::min(drawn, sat.page_drawn_lo[p]);
            }
            if (parts & SA_HI) {
                drawn = std::min(drawn, sat.page_drawn_hi[p]);
            }
            for (const SaOwner &o : sat.blocks[block].owners) {
                if (sa_and(o.cover, need).any()) {
                    newest = std::max(newest, o.seq);
                }
            }
            own = own && snt.page_named[p] == n.id && drawn != UINT64_MAX && drawn > newest;
        });
        if (!own) {
            continue;
        }
        if (snt.bound == &n) {
            st.reads_target = st.reads_own = true;
            return true;
        }
        if (!sat_refresh(n)) {
            return false;
        }
        st.src = tpsm == GSN_PSMCT24 ? SRC_TARGET_CT24 : psm_is_16(tpsm) ? SRC_TARGET_CT16 : SRC_TARGET_CT32;
        st.reads_target = true;
        st.target_view = &n.color->get_view();
        st.src_offset[0] = int32_t(x0);
        st.src_offset[1] = int32_t(y0);
        st.tscale = n.sx | n.sy << 16;
        sat_stats.textures++;
        return true;
    }
    return false;
}

// (sa_copied) A local copy whose source is all the table's uploads: made in the scratch VRAM, its destination
// an upload of its own. False: not all of the source is (the GS's VRAM then, as before).
bool sat_copied(uint64_t bitbltbuf, uint64_t trxpos, uint64_t trxreg) {
    if (snt.off) {
        return false;
    }
    uint32_t sbp = bits(bitbltbuf, 0, 14), sbw = bits(bitbltbuf, 16, 6), spsm = bits(bitbltbuf, 24, 6);
    uint32_t dpsm = bits(bitbltbuf, 56, 6);
    uint32_t sx = bits(trxpos, 0, 11), sy = bits(trxpos, 16, 11), w = bits(trxreg, 0, 12), h = bits(trxreg, 32, 12);
    if (!w || !h || !gsn_psm_valid(spsm) || !gsn_psm_valid(dpsm)) {
        return false;
    }
    // the source's owners, oldest first
    // (T4's table: every nibble of the source an upload's not drawn over since, sa_owners)
    std::vector<uint32_t> ids;
    std::vector<uint32_t> blocks;
    bool all = sa_owners(sbp, sbw, spsm, sx, sy, w, h, ids) == SA_TEX_TABLE;
    if (all) {
        sa_blocks_of(sbp, sbw, spsm, sx, sy, w, h, [&](uint32_t block, const SaCover &) { blocks.push_back(block); });
    }
    if (!all) {
        sat_stats.copies_not++;
        return false;
    }
    std::sort(ids.begin(), ids.end(), [](uint32_t a, uint32_t b) { return sa_chunk(a)->seq < sa_chunk(b)->seq; });
    if (sa.scratch.empty()) {
        sa.scratch.assign(GSN_VRAM_BYTES, 0);
    }
    uint8_t *vram = sa.scratch.data();
    for (uint32_t block : blocks) {
        memset(vram + size_t(block) * 256, 0, 256);
    }
    for (uint32_t id : ids) {
        const SaChunk &c = *sa_chunk(id);
        GsnUpload u;
        gsn_upload_begin(&u, c.bitbltbuf, c.trxpos, c.trxreg);
        gsn_upload_data(&u, vram, c.bytes.data(), c.bytes.size(), nullptr, nullptr);
    }
    gsn_vram_copy(vram, bitbltbuf, trxpos, trxreg);
    // the destination read back as an upload of it would send it (its source half made the destination's)
    size_t qwords = (size_t(w) * h * gsn_psm_bits(dpsm) + 127) / 128;
    std::vector<uint8_t> bytes(qwords * 16);
    uint64_t dst_bitbltbuf = bitbltbuf & ~uint64_t(0xFFFFFFFF), dst_trxpos = trxpos & ~uint64_t(0xFFFFFFFF);
    uint64_t as_source = dst_bitbltbuf | (dst_bitbltbuf >> 32);
    uint64_t pos = (dst_trxpos & ~(uint64_t(3) << 59)) >> 32;
    gsn_vram_read_image(vram, as_source, pos, trxreg, bytes.data(), qwords);
    // kept as an upload of the destination (not one of the frame's: a picture between does not ask for it)
    int recording = sa.recording;
    sa.recording = -1;
    uint64_t uploads = sa_stats.uploads, upload_bytes = sa_stats.upload_bytes;
    sa_uploaded(dst_bitbltbuf, dst_trxpos & ~(uint64_t(3) << 59), trxreg, bytes.data(), bytes.size());
    sa_stats.uploads = uploads;
    sa_stats.upload_bytes = upload_bytes;
    sa.recording = recording;
    // (its pages are no named target's)
    uint32_t lo, count;
    buffer_pages(bits(bitbltbuf, 32, 14), bits(bitbltbuf, 48, 6), dpsm, bits(trxpos, 32, 11) + w, bits(trxpos, 48, 11) + h, &lo,
                 &count);
    for (uint32_t k = 0; k < count && k < vram_pages; k++) {
        snt.page_named[(lo + k) % vram_pages] = 0;
    }
    sat_stats.copies++;
    sat_stats.copy_bytes += bytes.size();
    return true;
}

// (sa_switch on, sa_forget) Forgotten: VRAM is made anew
void sat_reset() {
    sat_release();
    snt.list.clear();
    memset(snt.page_named, 0, sizeof(snt.page_named));
    const char *off = getenv("KF4_SA_NO_NAMED");
    snt.off = off && atoi(off) > 0;
}

void sat_print_stats() {
    const SatStats &s = sat_stats;
    fprintf(stderr,
            "[stats] standalone targets: %llu named (%llu bound for the scene's draws, %llu cleared by the scene), %llu textures "
            "read from them; %llu made from the GS's, %llu given back to it (%llu not: a transfer wrote them whole); %llu copies made from the table (%.1f MB), %llu "
            "not; %llu fills drawn by the scene; %llu batches into the reflection left out by pictures between keeping the "
            "frame's\n",
            (unsigned long long)s.made, (unsigned long long)s.binds, (unsigned long long)s.clears, (unsigned long long)s.textures,
            (unsigned long long)s.refreshes, (unsigned long long)s.give_backs, (unsigned long long)s.overwritten, (unsigned long long)s.copies, s.copy_bytes / 1048576.0,
            (unsigned long long)s.copies_not, (unsigned long long)s.fills, (unsigned long long)sa_stats.kept_reflection);
}

// (sa_draw_range) The reflection's water in the scene: nd_water.vert built with ND_STANDALONE (sa_water.vert),
// with the GS's fragment shader (sample: a cut-out shaded at every sample, sa_sample.frag)
Program *sat_water_program(bool sample) {
    static Program *programs[2] = {};
    if (!programs[0]) {
        ResourceLayout vert = {};
        vert.input_mask = (1u << 2) | (1u << 4);
        vert.push_constant_size = sizeof(Push);
        vert.sets[1].storage_buffer_mask = 1u;
        vert.sets[1].meta[0].array_size = 1;
        vert.sets[2].storage_buffer_mask = 3u;
        vert.sets[2].meta[0].array_size = 1;
        vert.sets[2].meta[1].array_size = 1;
        Shader *vs = sa_jitter_on() // (upscaling: the jitter's own, as sa_programs)
                         ? gs->device->request_shader(gsn_spv_sa_water_jitter_vert, sizeof(gsn_spv_sa_water_jitter_vert), &vert)
                         : gs->device->request_shader(gsn_spv_sa_water_vert, sizeof(gsn_spv_sa_water_vert), &vert);
        programs[0] = gs->device->request_program(vs, gs->program->get_shader(ShaderStage::Fragment));
        programs[1] = sa.programs[SA_VS_000][1] ? gs->device->request_program(vs, sa.programs[SA_VS_000][1]->get_shader(ShaderStage::Fragment))
                                                : programs[0];
    }
    return programs[sample ? 1 : 0];
}

// (sa_flush) A fill over the frame the scene has (GsDraw_FillScreen's sprites: untextured, every pixel failing the
// alpha test into the frame only, Z not tested, any colour and blend): drawn by the scene as its own op, so it
// keeps the frame across it. Mode 1's mirror (041) covers its mirrored view with the map's tint so between its
// two views (Map_CoverScreenWithMapTint): with this the mirror, the tint, the depth clear and the real view are
// one turn of the scene's (and a fade over the world, GsDraw_FillScreen too, while the scene has the frame).
bool sat_fill() {
    snt.fill = false;
    if (snt.off || !gs->batch_sprites || nd.active || gs->verts.empty() || !sa.bound || snt.bound) {
        return false;
    }
    uint32_t prim = gs->batch_prim, ctxt = (prim >> 9) & 1;
    uint64_t test = gs->reg[R_TEST_1 + ctxt], frame = gs->reg[R_FRAME_1 + ctxt], zbuf = gs->reg[R_ZBUF_1 + ctxt];
    if (((prim >> 4) & 1) || !bits(test, 0, 1) || bits(test, 1, 3) != 0 || bits(test, 12, 2) != 1 || bits(test, 14, 1) ||
        (bits(test, 16, 1) && bits(test, 17, 2) != 1)) {
        return false;
    }
    snt.fill = bits(frame, 0, 9) * 32 == sa.fbp && bits(frame, 16, 6) == sa.fbw && bits(frame, 24, 6) == sa.fpsm &&
               bits(zbuf, 0, 9) * 32 == sa.zbp && (bits(zbuf, 24, 4) | 0x30) == sa.zpsm;
    sat_stats.fills += snt.fill;
    return snt.fill;
}

// (sa_draw_batch) A fill (sat_fill) is blended in the shader, from a copy of the scene's colour, as flush_batch
// blends a sprite: the GS's arithmetic, so a fade or a tint over the scene is what the GS would make of it
bool sat_blend_exact() {
    bool fill = snt.fill;
    snt.fill = false;
    return fill;
}
// (gsn_pc_draw.inc's snapshot) Whether sat_blend_exact would answer true, without taking the answer
bool sat_fill_waiting() {
    return snt.fill;
}
