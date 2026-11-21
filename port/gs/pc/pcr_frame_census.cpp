// KF4_PC_FRAME_CENSUS (pcr_frame_census.h): what each game frame hands the renderer
#include "pcr_frame_census.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "../../render/render_internal.hpp"

namespace pcr {

namespace {

uint32_t bits(uint64_t v, int at, int count) {
    return uint32_t((v >> at) & ((uint64_t(1) << count) - 1));
}

const char *kind_name(uint32_t kind) {
    static const char *names[] = {"?",       "map_record", "map_object", "item",   "gold",   "entity",
                                  "arm",     "effect",     "shadow",     "picked", "water",  "pass"};
    return kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "?";
}

std::string program_name(uint32_t program) {
    switch (program) {
    case RD_PROGRAM_GS_STRIP:
        return "strip";
    case RD_PROGRAM_RIPPLE:
        return "ripple";
    case RD_PROGRAM_PASS:
        return "pass";
    default: {
        char b[16];
        snprintf(b, sizeof(b), "%03x", program);
        return b;
    }
    }
}

// The GS state a draw is drawn with, less its texture: what a pipeline (and its dynamic state) is made of. program: a
// native batch's (NativeProgram + 1), 0 for GIF.
struct CensusPipeline {
    uint64_t frame, zbuf, test, alpha, misc;
    bool operator<(const CensusPipeline &o) const {
        return std::tie(frame, zbuf, test, alpha, misc) < std::tie(o.frame, o.zbuf, o.test, o.alpha, o.misc);
    }
    bool operator==(const CensusPipeline &o) const = default;
};

struct CensusTexture {
    uint64_t tex0 = 0, clamp = 0;
    bool operator<(const CensusTexture &o) const {
        return std::tie(tex0, clamp) < std::tie(o.tex0, o.clamp);
    }
    bool operator==(const CensusTexture &o) const = default;
};

CensusPipeline pipeline_of(const GsRegisterFile &file, uint32_t prim, uint32_t program) {
    const auto &r = file.reg;
    uint32_t c = bits(prim, 9, 1);
    uint64_t test = r[gs_reg::test_1 + c] & 0x7FFFF;
    bool zte = bits(test, 16, 1) != 0;
    CensusPipeline k{};
    k.frame = r[gs_reg::frame_1 + c] & 0x3F3F01FFull; // FBP, FBW, PSM
    k.frame |= uint64_t(bits(r[gs_reg::frame_1 + c], 32, 32) != 0) << 63; // (FBMSK: some bits masked)
    k.zbuf = zte ? (r[gs_reg::zbuf_1 + c] & 0x1'0F0001FFull) : 0;          // ZBP, PSM, ZMSK
    k.test = test;
    bool abe = bits(prim, 6, 1) != 0;
    k.alpha = abe ? (r[gs_reg::alpha_1 + c] & 0xFF'000000FFull) : 0;
    bool tme = bits(prim, 4, 1) != 0;
    uint64_t tex = tme ? (bits(r[gs_reg::tex0_1 + c], 34, 3) | uint64_t(bits(r[gs_reg::tex1_1 + c], 5, 4)) << 3) : 0;
    uint32_t shape = bits(prim, 0, 3);
    shape = shape == 6 ? 3 : shape >= 3 ? 2 : shape <= 0 ? 0 : 1; // point, line, triangle, sprite
    k.misc = uint64_t(shape) | uint64_t(bits(prim, 3, 8) & ~0x40u) << 2 | tex << 12 |
             uint64_t(bits(r[gs_reg::fba_1 + c], 0, 1)) << 24 | uint64_t(abe ? bits(r[gs_reg::pabe], 0, 1) : 0) << 25 |
             uint64_t(bits(r[gs_reg::colclamp], 0, 1)) << 26 | uint64_t(bits(r[gs_reg::dthe], 0, 1)) << 27 |
             uint64_t(program) << 32;
    return k;
}

CensusTexture texture_of(const GsRegisterFile &file, uint32_t prim) {
    if (bits(prim, 4, 1) == 0) {
        return {};
    }
    uint32_t c = bits(prim, 9, 1);
    uint64_t tex0 = file.reg[gs_reg::tex0_1 + c];
    uint32_t psm = bits(tex0, 20, 6);
    bool indexed = (psm & 0x7) >= 3; // PSMT8, T4, T8H, T4HL, T4HH
    // (TBP0, TBW, PSM, TW, TH; and CBP, CPSM, CSM, CSA for a CLUT's: not TCC, TFX or CLD)
    uint64_t mask = ((uint64_t(1) << 34) - 1) | (indexed ? ((uint64_t(1) << 24) - 1) << 37 : 0);
    return {tex0 & mask, file.reg[gs_reg::clamp_1 + c]};
}

struct Tally {
    uint64_t n = 0, pixels = 0;
};

} // namespace

class FrameCensus::Impl {
public:
    FILE *out = nullptr;
    std::vector<std::pair<uint64_t, uint64_t>> at; // (vblank ranges; empty: every stretch)

    // ---- the stretch's counts
    uint64_t path_bytes[5] = {};
    std::map<std::string, Tally> gif;        // "path class" -> primitives, pixels
    std::map<std::string, uint64_t> nd_draws, nd_batches, nd_triangles, nd_sphere;
    std::set<uint32_t> meshes, cameras;
    uint64_t nd_lost = 0;
    // the draws in order
    bool last_is_gif = false;
    CensusPipeline last_pipe{};
    CensusTexture last_tex{};
    uint64_t last_target = ~0ull;
    bool any = false;
    uint64_t draws = 0, gif_runs = 0, pipe_runs = 0, pipe_tex_runs = 0, target_runs = 0;
    std::set<CensusPipeline> pipes;
    std::set<CensusTexture> textures;
    std::set<uint64_t> targets, zbufs;
    // program 0x280's consecutive batches
    bool last_280 = false;
    uint64_t last_280_test = 0, last_280_alpha = 0, last_280_tex = 0, last_280_misc = 0, last_280_frame = 0;
    uint32_t last_280_instance = 0;
    uint64_t b280 = 0, runs_280 = 0;
    std::map<std::string, uint64_t> changes_280;
    std::map<std::string, uint64_t> native_by_pipe; // (program: batches whose pipeline differs from the batch before)
    // transfers
    uint64_t uploads = 0, upload_bytes = 0, uploads_again = 0, copies = 0, read_backs = 0;
    std::map<std::string, Tally> upload_to; // "psm@page" groups: texture, clut, target
    std::map<std::string, uint64_t> copy_kinds, marks;
    CanvasCounts canvas_before{};
    bool have_before = false;

    void draw(const CensusPipeline &p, const CensusTexture &t, bool gif_prim) {
        draws++;
        bool new_pipe = !any || !(p == last_pipe);
        bool new_tex = !any || !(t == last_tex);
        uint64_t target = p.frame & 0x3F3F01FFull;
        if (!any || target != last_target) {
            target_runs++;
        }
        if (new_pipe) {
            pipe_runs++;
        }
        if (new_pipe || new_tex) {
            pipe_tex_runs++;
        }
        if (gif_prim && (!any || !last_is_gif || new_pipe || new_tex)) {
            gif_runs++;
        }
        pipes.insert(p);
        if (t.tex0 || t.clamp) {
            textures.insert(t);
        }
        targets.insert(target);
        if (p.zbuf) {
            zbufs.insert(p.zbuf & 0x0F0001FFull);
        }
        last_pipe = p;
        last_tex = t;
        last_target = target;
        last_is_gif = gif_prim;
        any = true;
    }

    void reset() {
        CanvasCounts before = canvas_before;
        bool have = have_before;
        FILE *o = out;
        auto a = std::move(at);
        *this = Impl();
        out = o;
        at = std::move(a);
        canvas_before = before;
        have_before = have;
    }

    bool wanted(uint64_t v) const {
        if (at.empty()) {
            return true;
        }
        for (const auto &[lo, hi] : at) {
            if (v >= lo && v <= hi) {
                return true;
            }
        }
        return false;
    }

    template <class M>
    static void counts(FILE *f, const char *name, const M &m) {
        fprintf(f, "\"%s\":{", name);
        bool first = true;
        for (const auto &[k, v] : m) {
            fprintf(f, "%s\"%s\":%llu", first ? "" : ",", k.c_str(), (unsigned long long)v);
            first = false;
        }
        fprintf(f, "}");
    }

    void write(uint64_t vblank, const CanvasCounts &now) {
        uint64_t total = 0;
        for (uint64_t b : path_bytes) {
            total += b;
        }
        if (total == 0) {
            return;
        }
        FILE *f = out;
        fprintf(f, "{\"vblank\":%llu,\"bytes\":[%llu,%llu,%llu,%llu]", (unsigned long long)vblank,
                (unsigned long long)path_bytes[0], (unsigned long long)path_bytes[1], (unsigned long long)path_bytes[2],
                (unsigned long long)path_bytes[3]);
        fprintf(f, ",\"gif\":{");
        bool first = true;
        for (const auto &[k, t] : gif) {
            fprintf(f, "%s\"%s\":[%llu,%llu]", first ? "" : ",", k.c_str(), (unsigned long long)t.n,
                    (unsigned long long)t.pixels);
            first = false;
        }
        fprintf(f, "},\"native\":{");
        counts(f, "draws", nd_draws);
        fprintf(f, ",");
        counts(f, "batches", nd_batches);
        fprintf(f, ",");
        counts(f, "triangles", nd_triangles);
        fprintf(f, ",");
        counts(f, "sphere", nd_sphere);
        fprintf(f, ",\"meshes\":%zu,\"cameras\":%zu,\"lost\":%llu}", meshes.size(), cameras.size(),
                (unsigned long long)nd_lost);
        fprintf(f,
                ",\"order\":{\"draws\":%llu,\"gif_runs\":%llu,\"pipeline_runs\":%llu,\"pipeline_texture_runs\":%llu,"
                "\"target_runs\":%llu,\"pipelines\":%zu,\"textures\":%zu,\"targets\":[",
                (unsigned long long)draws, (unsigned long long)gif_runs, (unsigned long long)pipe_runs,
                (unsigned long long)pipe_tex_runs, (unsigned long long)target_runs, pipes.size(), textures.size());
        first = true;
        for (uint64_t t : targets) {
            fprintf(f, "%s\"fbp %03x fbw %u psm %02x\"", first ? "" : ",", bits(t, 0, 9), bits(t, 16, 6), bits(t, 24, 6));
            first = false;
        }
        fprintf(f, "],\"zbufs\":[");
        first = true;
        for (uint64_t z : zbufs) {
            fprintf(f, "%s\"zbp %03x psm %02x\"", first ? "" : ",", bits(z, 0, 9), bits(z, 24, 4) | 0x30);
            first = false;
        }
        fprintf(f, "],");
        counts(f, "native_pipeline_changes", native_by_pipe);
        fprintf(f, "},\"p280\":{\"batches\":%llu,\"runs\":%llu,", (unsigned long long)b280, (unsigned long long)runs_280);
        counts(f, "changes", changes_280);
        fprintf(f, "},\"transfers\":{\"uploads\":%llu,\"upload_bytes\":%llu,\"again\":%llu,\"copies\":%llu,"
                   "\"read_backs\":%llu,\"to\":{",
                (unsigned long long)uploads, (unsigned long long)upload_bytes, (unsigned long long)uploads_again,
                (unsigned long long)copies, (unsigned long long)read_backs);
        first = true;
        for (const auto &[k, t] : upload_to) {
            fprintf(f, "%s\"%s\":[%llu,%llu]", first ? "" : ",", k.c_str(), (unsigned long long)t.n,
                    (unsigned long long)t.pixels);
            first = false;
        }
        fprintf(f, "},");
        counts(f, "copy_kinds", copy_kinds);
        fprintf(f, "},");
        counts(f, "marks", marks);
        if (have_before) {
            const CanvasCounts &b = canvas_before;
            fprintf(f,
                    ",\"canvas\":{\"batches\":%llu,\"native_batches\":%llu,\"native_draw_calls\":%llu,\"passes\":%llu,"
                    "\"clears\":%llu,\"date_draws\":%llu,\"textures_from_surfaces\":%llu,\"textures_uploaded\":%llu,"
                    "\"surfaces_made\":%llu,\"copies_between_surfaces\":%llu,\"triangles\":%llu}",
                    (unsigned long long)(now.batches - b.batches), (unsigned long long)(now.native_batches - b.native_batches),
                    (unsigned long long)(now.native_draw_calls - b.native_draw_calls),
                    (unsigned long long)(now.passes - b.passes), (unsigned long long)(now.clears - b.clears),
                    (unsigned long long)(now.date_draws - b.date_draws),
                    (unsigned long long)(now.textures_from_surfaces - b.textures_from_surfaces),
                    (unsigned long long)(now.textures_uploaded - b.textures_uploaded),
                    (unsigned long long)(now.surfaces_made - b.surfaces_made),
                    (unsigned long long)(now.copies_between_surfaces - b.copies_between_surfaces),
                    (unsigned long long)(now.triangles - b.triangles));
        }
        fprintf(f, "}\n");
        fflush(f);
    }
};

FrameCensus::FrameCensus() : impl_(std::make_unique<Impl>()) {}

FrameCensus::~FrameCensus() {
    if (impl_->out) {
        fclose(impl_->out);
    }
}

std::unique_ptr<FrameCensus> FrameCensus::from_env() {
    const char *path = getenv("KF4_PC_FRAME_CENSUS");
    if (!path || !*path) {
        return nullptr;
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "[pcr] KF4_PC_FRAME_CENSUS: cannot write %s\n", path);
        return nullptr;
    }
    std::unique_ptr<FrameCensus> c(new FrameCensus());
    c->impl_->out = f;
    if (const char *at = getenv("KF4_PC_FRAME_CENSUS_AT")) {
        std::string s = at;
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(',', p);
            std::string part = s.substr(p, e == std::string::npos ? std::string::npos : e - p);
            unsigned long long lo = 0, hi = 0;
            int n = sscanf(part.c_str(), "%llu-%llu", &lo, &hi);
            if (n >= 1) {
                c->impl_->at.push_back({lo, n == 2 ? hi : lo});
            }
            if (e == std::string::npos) {
                break;
            }
            p = e + 1;
        }
    }
    fprintf(stderr, "[pcr] KF4_PC_FRAME_CENSUS: each frame's census into %s (developers' only)\n", path);
    return c;
}

void FrameCensus::bytes(int path, size_t n) {
    impl_->path_bytes[std::clamp(path, 0, 4)] += n;
}

void FrameCensus::primitive(int path, bool native, const GsRegisterFile &file, const GifPrimitive &p) {
    Impl &m = *impl_;
    const auto &r = file.reg;
    uint32_t c = bits(p.prim, 9, 1);
    uint64_t test = r[gs_reg::test_1 + c];
    bool zte = bits(test, 16, 1) != 0;
    uint32_t ztst = bits(test, 17, 2);
    static const char *shapes[] = {"point", "line", "tri", "sprite"};
    char key[160];
    snprintf(key, sizeof(key), "%s %s%s%s%s%s%s", native ? "native" : path == 1 ? "p1" : path == 2 ? "p2" : "p3",
             shapes[int(p.shape)], bits(p.prim, 4, 1) ? " tex" : "", bits(p.prim, 6, 1) ? " blend" : "",
             zte && ztst >= 2 ? " ztest" : zte && ztst == 1 && !bits(r[gs_reg::zbuf_1 + c], 32, 1) ? " zwrite" : "",
             bits(test, 14, 1) ? " date" : "",
             bits(test, 0, 1) && bits(test, 1, 3) == 0 ? " atst_never" : "");
    // (its rectangle in the scissor, GS pixels)
    int ox = int(bits(r[gs_reg::xyoffset_1 + c], 0, 16)), oy = int(bits(r[gs_reg::xyoffset_1 + c], 32, 16));
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (uint32_t i = 0; i < p.vertex_count && i < 3; i++) {
        int x = (int(p.vertices[i].x) - ox) >> 4, y = (int(p.vertices[i].y) - oy) >> 4;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    uint64_t sc = r[gs_reg::scissor_1 + c];
    x0 = std::max(x0, int(bits(sc, 0, 11)));
    x1 = std::min(x1, int(bits(sc, 16, 11)) + 1);
    y0 = std::max(y0, int(bits(sc, 32, 11)));
    y1 = std::min(y1, int(bits(sc, 48, 11)) + 1);
    uint64_t px = x1 > x0 && y1 > y0 ? uint64_t(x1 - x0) * uint64_t(y1 - y0) : 0;
    if (p.shape == PrimitiveShape::triangle) {
        px /= 2; // (a triangle's half of its rectangle, near enough)
    }
    Tally &t = m.gif[key];
    t.n++;
    t.pixels += px;
    m.draw(pipeline_of(file, p.prim, 0), texture_of(file, p.prim), true);
    m.last_280 = false;
}

void FrameCensus::native_draw(uint64_t value) {
    Impl &m = *impl_;
    if (RD_ND_COMMAND(value) != RD_ND_DRAW) {
        char b[32];
        snprintf(b, sizeof(b), "nd %02x", RD_ND_COMMAND(value));
        m.marks[b]++;
        return;
    }
    RdDraw d;
    RdCamera cam;
    std::vector<std::shared_ptr<const RdMeshData>> meshes;
    if (!rd_fetch(value, d, cam, meshes)) {
        m.nd_lost++;
        return;
    }
    std::string key = std::string(kind_name(d.kind)) + " " + program_name(d.program);
    if (d.program == RD_PROGRAM_PASS) {
        RdPass pass;
        rd_pass_of(d, pass);
        char b[48];
        snprintf(b, sizeof(b), "pass %u site %u", pass.pass, pass.site);
        key = b;
    }
    m.nd_draws[key]++;
    m.cameras.insert(d.camera);
    for (const auto &mesh : meshes) {
        if (!mesh) {
            continue;
        }
        m.meshes.insert(mesh->id);
        for (const RdBatch &b : mesh->batches) {
            m.nd_batches[key]++;
            m.nd_triangles[key] += b.triangles.size() / 3;
            if (b.sphere) {
                m.nd_sphere[key]++;
            }
        }
    }
}

void FrameCensus::native_batch(const GsRegisterFile &file, uint32_t prim, const NativeBatch &nb) {
    Impl &m = *impl_;
    uint32_t program = uint32_t(nb.program) + 1;
    CensusPipeline p = pipeline_of(file, prim, program);
    CensusTexture t = texture_of(file, prim);
    bool new_pipe = !m.any || !(p == m.last_pipe);
    m.draw(p, t, false);
    static const char *names[] = {"000", "280", "700", "ripple"};
    std::string name = names[std::min<uint32_t>(uint32_t(nb.program), 3)];
    m.native_by_pipe[name + " batches"]++;
    if (new_pipe) {
        m.native_by_pipe[name + " pipeline changes"]++;
    }
    if (nb.program == NativeProgram::vu1_280) {
        uint32_t c = bits(prim, 9, 1);
        uint64_t test = file.reg[gs_reg::test_1 + c], alpha = bits(prim, 6, 1) ? file.reg[gs_reg::alpha_1 + c] : 0;
        uint64_t frame = file.reg[gs_reg::frame_1 + c];
        m.b280++;
        if (!m.last_280) {
            m.runs_280++;
            m.changes_280["(after another draw)"]++;
        } else {
            bool any = false;
            auto note = [&](bool changed, const char *what) {
                if (changed) {
                    m.changes_280[what]++;
                    any = true;
                }
            };
            note(test != m.last_280_test, "test");
            note(alpha != m.last_280_alpha, "alpha");
            note(t.tex0 != m.last_280_tex, "texture");
            note(p.misc != m.last_280_misc, "prim");
            note(frame != m.last_280_frame, "frame");
            note(((nb.instance >> 29) & 1) != ((m.last_280_instance >> 29) & 1), "sphere pass");
            note((nb.instance & 0x0FFFFFFF) != (m.last_280_instance & 0x0FFFFFFF), "record");
            if (!any) {
                m.changes_280["nothing"]++;
            }
        }
        if (m.last_280 && new_pipe) {
            m.runs_280++;
        }
        m.last_280 = true;
        m.last_280_test = test;
        m.last_280_alpha = alpha;
        m.last_280_tex = t.tex0;
        m.last_280_misc = p.misc;
        m.last_280_frame = frame;
        m.last_280_instance = nb.instance;
    } else {
        m.last_280 = false;
    }
}

void FrameCensus::upload(const TransferRegisters &t, size_t bytes) {
    Impl &m = *impl_;
    m.uploads++;
    m.upload_bytes += bytes;
    uint32_t dbp = bits(t.bitbltbuf, 32, 14), dpsm = bits(t.bitbltbuf, 56, 6);
    char k[48];
    snprintf(k, sizeof(k), "psm %02x %s", dpsm, dbp / 32 < 350 ? "below 350" : "350+");
    m.upload_to[k].n++;
    m.upload_to[k].pixels += bytes;
}

void FrameCensus::upload_again() {
    impl_->uploads_again++;
}

void FrameCensus::local_copy(const TransferRegisters &t) {
    Impl &m = *impl_;
    m.copies++;
    char k[64];
    snprintf(k, sizeof(k), "%03x->%03x %ux%u", bits(t.bitbltbuf, 0, 14) / 32, bits(t.bitbltbuf, 32, 14) / 32,
             bits(t.trxreg, 0, 12), bits(t.trxreg, 32, 12));
    m.copy_kinds[k]++;
}

void FrameCensus::read_back(const TransferRegisters &t) {
    Impl &m = *impl_;
    m.read_backs++;
    char k[64];
    snprintf(k, sizeof(k), "read back %03x %ux%u", bits(t.bitbltbuf, 0, 14) / 32, bits(t.trxreg, 0, 12),
             bits(t.trxreg, 32, 12));
    m.copy_kinds[k]++;
}

void FrameCensus::frame_mark(uint64_t value) {
    char b[32];
    snprintf(b, sizeof(b), "7d %02x", uint32_t(value >> 32) & 0xFF);
    impl_->marks[b]++;
}

void FrameCensus::vblank(uint64_t game_vblank, const CanvasCounts &canvas) {
    Impl &m = *impl_;
    if (m.wanted(game_vblank)) {
        m.write(game_vblank, canvas);
    }
    m.canvas_before = canvas;
    m.have_before = true;
    m.reset();
}

} // namespace pcr
