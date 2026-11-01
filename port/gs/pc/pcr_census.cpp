// KF4_PC_CENSUS=1 (diagnostic, --renderer pc): what the game sends the GS, counted by kind, printed at exit:
// the targets drawn into, the tests and blends, the textures sampled, the primitives, the transfers. The PC
// renderer is built for what is here (docs/port/PC_RENDERER.md, "The PC renderer: `fast` without the native GS").
#include "pcr_census.h"

#include <stdio.h>

#include <algorithm>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace pcr {

namespace {

uint32_t bits(uint64_t v, int at, int count) {
    return uint32_t((v >> at) & ((uint64_t(1) << count) - 1));
}

} // namespace

class Census::Tally {
public:
    void add(const std::string &table, const std::string &key) {
        tables_[table][key]++;
    }

    void print(FILE *out) const {
        for (const auto &[name, rows] : tables_) {
            std::vector<std::pair<uint64_t, std::string>> sorted;
            uint64_t total = 0;
            for (const auto &[key, n] : rows) {
                sorted.emplace_back(n, key);
                total += n;
            }
            std::sort(sorted.rbegin(), sorted.rend());
            fprintf(out, "[census] %s: %zu kinds, %llu in all\n", name.c_str(), sorted.size(), (unsigned long long)total);
            for (size_t i = 0; i < sorted.size() && i < 48; i++) {
                fprintf(out, "[census]   %10llu  %s\n", (unsigned long long)sorted[i].first, sorted[i].second.c_str());
            }
        }
    }

private:
    std::map<std::string, std::map<std::string, uint64_t>> tables_;
};

Census::Census() : tally_(std::make_unique<Tally>()) {}
Census::~Census() = default;

void Census::primitive(const GsRegisterFile &file, const GifPrimitive &p) {
    const auto &r = file.reg;
    uint32_t ctxt = bits(p.prim, 9, 1);
    uint64_t frame = r[0x4C + ctxt], zbuf = r[0x4E + ctxt], test = r[0x47 + ctxt], alpha = r[0x42 + ctxt];
    uint64_t tex0 = r[0x06 + ctxt], tex1 = r[0x14 + ctxt], clamp = r[0x08 + ctxt];
    char k[256];
    static const char *shapes[] = {"point", "line", "triangle", "sprite"};
    snprintf(k, sizeof(k), "%s type %u IIP %u TME %u FGE %u ABE %u AA1 %u FST %u CTXT %u", shapes[int(p.shape)],
             bits(p.prim, 0, 3), bits(p.prim, 3, 1), bits(p.prim, 4, 1), bits(p.prim, 5, 1), bits(p.prim, 6, 1),
             bits(p.prim, 7, 1), bits(p.prim, 8, 1), ctxt);
    tally_->add("primitives", k);
    bool zte = bits(test, 16, 1) != 0;
    bool zwrite = zte && bits(zbuf, 32, 1) == 0;
    snprintf(k, sizeof(k), "FBP %03x FBW %2u PSM %02x FBMSK %08x | Z %s ZBP %03x ZPSM %02x ZMSK %u", bits(frame, 0, 9),
             bits(frame, 16, 6), bits(frame, 24, 6), bits(frame, 32, 32), zte ? "on" : "off", bits(zbuf, 0, 9),
             bits(zbuf, 24, 4), bits(zbuf, 32, 1));
    tally_->add("targets", k);
    (void)zwrite;
    snprintf(k, sizeof(k), "ATE %u ATST %u AREF %02x AFAIL %u DATE %u DATM %u ZTE %u ZTST %u", bits(test, 0, 1),
             bits(test, 1, 3), bits(test, 4, 8), bits(test, 12, 2), bits(test, 14, 1), bits(test, 15, 1),
             bits(test, 16, 1), bits(test, 17, 2));
    tally_->add("tests", k);
    if (bits(p.prim, 6, 1)) {
        snprintf(k, sizeof(k), "A %u B %u C %u D %u FIX %02x PABE %u FBA %u COLCLAMP %u DTHE %u", bits(alpha, 0, 2),
                 bits(alpha, 2, 2), bits(alpha, 4, 2), bits(alpha, 6, 2), bits(alpha, 32, 8), bits(r[0x49], 0, 1),
                 bits(r[0x4A + ctxt], 0, 1), bits(r[0x46], 0, 1), bits(r[0x45], 0, 1));
        tally_->add("blends", k);
    } else {
        snprintf(k, sizeof(k), "FBA %u COLCLAMP %u DTHE %u", bits(r[0x4A + ctxt], 0, 1), bits(r[0x46], 0, 1),
                 bits(r[0x45], 0, 1));
        tally_->add("no blend", k);
    }
    if (bits(p.prim, 4, 1)) {
        snprintf(k, sizeof(k), "PSM %02x TW %u TH %u TCC %u TFX %u CPSM %u CSM %u | MXL %u MMAG %u MMIN %u LCM %u K %03x | "
                 "WMS %u WMT %u",
                 bits(tex0, 20, 6), bits(tex0, 26, 4), bits(tex0, 30, 4), bits(tex0, 34, 1), bits(tex0, 35, 2),
                 bits(tex0, 51, 4), bits(tex0, 55, 1), bits(tex1, 2, 3), bits(tex1, 5, 1), bits(tex1, 6, 3),
                 bits(tex1, 0, 1), bits(tex1, 32, 12), bits(clamp, 0, 2), bits(clamp, 2, 2));
        tally_->add("textures", k);
        // the texture's base against the targets drawn into so far (a surface sampled)
        uint32_t tbp = bits(tex0, 0, 14);
        auto t = drawn_.upper_bound(tbp);
        bool on_target = false;
        if (t != drawn_.begin()) {
            --t;
            on_target = tbp < t->second;
        }
        snprintf(k, sizeof(k), "TBP %04x PSM %02x TBW %2u %s", tbp, bits(tex0, 20, 6), bits(tex0, 14, 6),
                 on_target ? "ON A TARGET DRAWN" : "uploads");
        tally_->add("texture bases", k);
    }
    // the target's pages drawn into, roughly: FBP's page to the scissor's bottom
    uint64_t scissor = r[0x40 + ctxt];
    uint32_t fbp = bits(frame, 0, 9), fbw = std::max(1u, bits(frame, 16, 6)), bottom = bits(scissor, 48, 11) + 1;
    uint32_t page_h = bits(frame, 24, 6) == 0x02 || bits(frame, 24, 6) == 0x0A ? 64 : 32;
    uint32_t end = (fbp + fbw * ((bottom + page_h - 1) / page_h)) * 32;
    auto &e = drawn_[fbp * 32];
    e = std::max(e, end);
}

void Census::upload(const TransferRegisters &t, size_t bytes) {
    char k[160];
    snprintf(k, sizeof(k), "DBP %04x DBW %2u DPSM %02x %ux%u", bits(t.bitbltbuf, 32, 14), bits(t.bitbltbuf, 48, 6),
             bits(t.bitbltbuf, 56, 6), bits(t.trxreg, 0, 12), bits(t.trxreg, 32, 12));
    tally_->add("uploads", k);
    (void)bytes;
}

void Census::copy(const TransferRegisters &t, const char *kind) {
    char k[200];
    snprintf(k, sizeof(k), "%s SBP %04x SBW %2u SPSM %02x (%u,%u) -> DBP %04x DBW %2u DPSM %02x (%u,%u) %ux%u", kind,
             bits(t.bitbltbuf, 0, 14), bits(t.bitbltbuf, 16, 6), bits(t.bitbltbuf, 24, 6), bits(t.trxpos, 0, 11),
             bits(t.trxpos, 16, 11), bits(t.bitbltbuf, 32, 14), bits(t.bitbltbuf, 48, 6), bits(t.bitbltbuf, 56, 6),
             bits(t.trxpos, 32, 11), bits(t.trxpos, 48, 11), bits(t.trxreg, 0, 12), bits(t.trxreg, 32, 12));
    tally_->add("copies", k);
}

void Census::other(const char *what) {
    tally_->add("other", what);
}

void Census::display(uint64_t pmode, uint64_t dispfb1, uint64_t dispfb2) {
    char k[160];
    snprintf(k, sizeof(k), "PMODE EN %u ALP %02x | FB1 FBP %03x FBW %u PSM %02x | FB2 FBP %03x FBW %u PSM %02x",
             bits(pmode, 0, 2), bits(pmode, 8, 8), bits(dispfb1, 0, 9), bits(dispfb1, 9, 6), bits(dispfb1, 15, 5),
             bits(dispfb2, 0, 9), bits(dispfb2, 9, 6), bits(dispfb2, 15, 5));
    tally_->add("display", k);
}

void Census::print() const {
    tally_->print(stderr);
}

} // namespace pcr
