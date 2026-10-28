// ---- Textures for the GPU's sampler (--native-sampler hardware, docs/port/PC_RENDERER.md P1) ---------------------------
// (included by gsn_standalone.inc after the table's textures, in gsn.cpp's namespace; the GS's thread)
//
// gs_frag.h reads a texture as the GS's texture unit does: its raw texels, each read wrapped as CLAMP says and put through
// the CLUT, bilinear four such reads, trilinear eight, --native-aniso 16 up to sixteen trilinear. At the owner's settings
// that is most of the scene's colour draws' GPU time (PC_RENDERER.md, "Why"). Here a table texture (sa_table_texture) is
// made RGBA8 once, with its mip levels: each texel decoded on the CPU as texel_f decodes it (the CLUT in use, CT32, CT24
// and CT16 with TEXA), the GS's byte values in each channel (alpha 0x80 opaque), uploaded as the raw texels are, and kept
// for each (texture, CLUT, TEXA) as the decoded textures are kept. The scene's batches then read it with Vulkan's
// sampler: gs_frag.h built with GSN_HW_SAMPLER (gs_hw.frag & co.), binding 6.
//
// What changes in the picture is the filtering only: the GPU's bilinear (8 bits of fraction, not the GS's 4), and the
// mip level from the pixel's footprint rather than the GS's K + log2(1/Q) << L. A texture the stock samplers cannot
// read as the GS does keeps gs_frag.h's own sampling: a render target, CLAMP's region clamp or repeat, S and T wrapped
// differently.

// Whether the GPU's sampler stands in for the GS's for this batch's texture, and with which sampler
struct RgbaSampling {
    bool usable = false;
    const char *why_not = "off"; // (usable false) a constant string, for --stats
    StockSampler sampler = StockSampler::NearestClamp;
};

struct Rgba {
    struct Entry {
        ImageHandle image;
        uint64_t used = 0; // (sa.frame when last drawn)
    };
    std::unordered_map<uint64_t, Entry> made; // by rgba_key
    std::deque<uint64_t> order;
    size_t bytes = 0;
    int aniso = -1; // the anisotropy the geometry samplers were made with (configure_default_geometry_samplers)
    // --stats
    uint64_t expansions = 0, hits = 0, texels = 0, batches = 0;
    std::unordered_map<const char *, uint64_t> kept_gs; // batches that kept the GS's sampling, by why
} rgba;

RgbaSampling rgba_sampling(const SaTexture &st, uint64_t tex1, uint64_t clamp) {
    RgbaSampling s;
    if (!hw_sampler) {
        return s;
    }
    if (!st.table || !st.decoded || st.decoded->raw.empty()) {
        s.why_not = "not from the table";
        return s;
    }
    if (st.src >= SRC_TARGET_CT32) {
        s.why_not = "a render target";
        return s;
    }
    uint32_t wms = bits(clamp, 0, 2), wmt = bits(clamp, 2, 2);
    if (wms >= 2 || wmt >= 2) {
        s.why_not = "region clamp or repeat";
        return s;
    }
    if (wms != wmt) {
        s.why_not = "S and T wrapped differently";
        return s;
    }
    bool wrap = wms == 0;
    // TEX1: MMAG (bit 5), MMIN (bits 6-8): 0 nearest, 1 linear, 2-5 mipmapped (2 nearest/nearest level, 3 nearest/
    // between levels, 4 linear/nearest level, 5 linear/between levels). The stock samplers filter the same in and out:
    // a texture is read as it is magnified (MMAG), and between levels where MMIN says so.
    bool linear = bits(tex1, 5, 1) != 0;
    uint32_t mmin = bits(tex1, 6, 3);
    bool between_levels = mmin == 3 || mmin == 5;
    s.usable = true;
    s.why_not = nullptr;
    if (!linear) {
        s.sampler = wrap ? StockSampler::NearestWrap : StockSampler::NearestClamp;
    } else if (between_levels) {
        // (with --native-aniso's anisotropy: rgba_configure)
        s.sampler = wrap ? StockSampler::DefaultGeometryFilterWrap : StockSampler::DefaultGeometryFilterClamp;
    } else {
        s.sampler = wrap ? StockSampler::LinearWrap : StockSampler::LinearClamp;
    }
    return s;
}

// The geometry samplers made with --native-aniso's anisotropy, before the first draw that uses them and again when it
// changes: the back end's thread looks the stock samplers up as it makes the calls, so it is drained first
void rgba_configure() {
    int aniso = 1 << aniso_log2;
    if (aniso == rgba.aniso) {
        return;
    }
    backend_drain();
    gs->device->configure_default_geometry_samplers(float(aniso), 0.0f);
    rgba.aniso = aniso;
}

uint64_t rgba_key(const Sa::Decoded &d, const SaTexture &st, uint32_t texa) {
    uint64_t key = (d.serial ^ uint64_t(st.src) << 56) * 0x100000001B3ull;
    if (st.src == SRC_CLUT && st.want_clut) {
        for (uint32_t e : st.clut) {
            key = (key ^ e) * 0x100000001B3ull;
        }
    } else if (st.src == SRC_CT24 || st.src == SRC_CT16) {
        key = (key ^ texa) * 0x100000001B3ull;
    }
    return key ^ (key >> 29);
}

// One texel as gs_frag.h's texel_f gives it for a texture that is not a target, as RGBA8's bytes (R lowest)
uint32_t rgba_texel(uint32_t raw, const SaTexture &st, uint32_t texa) {
    auto texa_alpha = [&](uint32_t rgb, bool bit, bool has_bit) -> uint32_t {
        if (has_bit && bit) {
            return (texa >> 16) & 0xFF;
        }
        if ((texa & 0x8000) && rgb == 0) {
            return 0;
        }
        return texa & 0xFF;
    };
    switch (st.src) {
    case SRC_CLUT:
        return st.want_clut ? st.clut[raw & 0xFF] : 0;
    case SRC_CT32:
        return raw;
    case SRC_CT24:
        return (raw & 0xFFFFFF) | texa_alpha(raw & 0xFFFFFF, false, false) << 24;
    default: { // (CT16)
        uint32_t rgb = (raw & 0x1F) << 3 | ((raw >> 5) & 0x1F) << 11 | ((raw >> 10) & 0x1F) << 19;
        return rgb | texa_alpha(rgb, (raw & 0x8000) != 0, true) << 24;
    }
    }
}

// The RGBA8 view of st's texture with the CLUT and TEXA of this batch: made (and uploaded into the stream, as a
// decoded texture is) the first time this triple is drawn
const ImageView &rgba_view(const SaTexture &st, uint32_t texa) {
    const Sa::Decoded &d = *st.decoded;
    uint64_t key = rgba_key(d, st, texa);
    auto found = rgba.made.find(key);
    if (found != rgba.made.end()) {
        rgba.hits++;
        found->second.used = sa.frame;
        return found->second.image->get_view();
    }
    Timer timer(gsn_stats.ns_decode);
    Rgba::Entry e;
    e.image = make_image(d.width, d.height, VK_FORMAT_R8G8B8A8_UNORM, d.levels, false);
    e.used = sa.frame;
    {
        Why why_(GSN_WHY_DECODE);
        end_pass();
    }
    transfer_barrier();
    size_t at = 0;
    for (uint32_t l = 0; l < d.levels; l++) {
        uint32_t w = std::max(1u, d.width >> l), h = std::max(1u, d.height >> l);
        auto *out = static_cast<uint32_t *>(
            cmd().update_image(*e.image, {0, 0, 0}, {w, h, 1}, w, h, {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1}));
        for (size_t i = 0; i < size_t(w) * h; i++) {
            out[i] = rgba_texel(d.raw[at + i], st, texa);
        }
        at += size_t(w) * h;
        rgba.texels += size_t(w) * h;
    }
    transfer_barrier();
    rgba.expansions++;
    rgba.bytes += image_bytes(*e.image);
    rgba.order.push_back(key);
    auto &slot = rgba.made[key] = std::move(e);
    // the oldest let go beyond 4,096 of them or 256 MB (a recording that names one holds its image all the same)
    while (rgba.order.size() > 4096 || rgba.bytes > (256u << 20)) {
        auto oldest = rgba.made.find(rgba.order.front());
        if (oldest != rgba.made.end() && &oldest->second != &slot) {
            rgba.bytes -= image_bytes(*oldest->second.image);
            rgba.made.erase(oldest);
        }
        rgba.order.pop_front();
    }
    return slot.image->get_view();
}

// A batch that kept the GS's sampling, and why (--stats)
void rgba_kept_gs(const RgbaSampling &s) {
    if (hw_sampler) {
        rgba.kept_gs[s.why_not]++;
    }
}

void rgba_forget() {
    rgba.made.clear();
    rgba.order.clear();
    rgba.bytes = 0;
}

void rgba_print_stats() {
    if (!hw_sampler) {
        return;
    }
    fprintf(stderr,
            "[stats] native sampler hardware: %llu batches read RGBA8 textures (%llu made, %.1f Mtexels; %llu found again; "
            "%zu kept, %.1f MB); the GS's sampling kept:",
            (unsigned long long)rgba.batches, (unsigned long long)rgba.expansions, double(rgba.texels) / 1e6,
            (unsigned long long)rgba.hits, rgba.made.size(), double(rgba.bytes) / 1048576.0);
    for (const auto &k : rgba.kept_gs) {
        fprintf(stderr, " %s %llu,", k.first, (unsigned long long)k.second);
    }
    fprintf(stderr, "\n");
}
