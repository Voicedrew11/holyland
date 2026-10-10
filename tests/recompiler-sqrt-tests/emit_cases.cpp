// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2recomp/code_generator.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/Translators/fpu_translator.h"
#include "ps2recomp/types.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {
constexpr uint32_t op(unsigned fd, unsigned fs, unsigned ft, unsigned function) {
    return 0x46000000u | (ft << 16u) | (fs << 11u) | (fd << 6u) | function;
}
struct Case { const char* name; uint32_t word; unsigned fd, fs, ft, function; };
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::vector<ps2recomp::Symbol> symbols;
    const std::vector<ps2recomp::Section> sections;
    ps2recomp::CodeGenerator generator(symbols, sections);
    ps2recomp::FpuTranslator translator(generator);
    ps2recomp::R5900Decoder decoder;
    const std::array cases{
        Case{"sqrt_distance", 0x460C0004u, 0u, 0u, 12u, 4u},
        Case{"sqrt_distinct", op(21, 7, 12, 4), 21u, 7u, 12u, 4u},
        Case{"sqrt_alias_ft", op(12, 7, 12, 4), 12u, 7u, 12u, 4u},
        Case{"sqrt_alias_fs", op(7, 7, 12, 4), 7u, 7u, 12u, 4u},
        Case{"rsqrt_distinct", op(3, 4, 5, 0x16), 3u, 4u, 5u, 0x16u},
        Case{"rsqrt_alias_ft", op(5, 4, 5, 0x16), 5u, 4u, 5u, 0x16u},
        Case{"rsqrt_alias_fs", op(4, 4, 5, 0x16), 4u, 4u, 5u, 0x16u},
        Case{"add_unchanged", op(21, 7, 12, 0), 21u, 7u, 12u, 0u},
        Case{"sub_unchanged", op(21, 7, 12, 1), 21u, 7u, 12u, 1u},
        Case{"mul_unchanged", op(21, 7, 12, 2), 21u, 7u, 12u, 2u},
        Case{"abs_unchanged", op(21, 7, 12, 5), 21u, 7u, 12u, 5u},
        Case{"mov_unchanged", op(21, 7, 12, 6), 21u, 7u, 12u, 6u},
        Case{"neg_unchanged", op(21, 7, 12, 7), 21u, 7u, 12u, 7u},
        Case{"edge_square_x", op(1, 20, 20, 2), 1u, 20u, 20u, 2u},
        Case{"edge_square_z", op(0, 21, 21, 2), 0u, 21u, 21u, 2u},
        Case{"edge_sum", op(12, 0, 1, 0), 12u, 0u, 1u, 0u},
        Case{"edge_normalize_x", op(2, 20, 0, 3), 2u, 20u, 0u, 3u},
        Case{"edge_normalize_z", op(1, 21, 0, 3), 1u, 21u, 0u, 3u},
    };
    std::ofstream output(argv[1]);
    output << "// Synthetic raw opcodes translated by actual R5900Decoder and FpuTranslator.\n"
              "#pragma once\n#include \"ps2_runtime_macros.h\"\n";
    for (const auto& item : cases) {
        const auto decoded = decoder.decodeInstruction(0x100000u, item.word, false);
        if (decoded.rs != 16 || decoded.sa != item.fd || decoded.rd != item.fs ||
            decoded.rt != item.ft || decoded.function != item.function) {
            std::fprintf(stderr, "Unexpected decoded fields for %s\n", item.name);
            return 3;
        }
        const std::string code = translator.translate(decoded);
        if (code.empty() || code.find("Unhandled") != std::string::npos) return 4;
        output << "inline void " << item.name << "(R5900Context* ctx) { " << code << " }\n";
    }
    std::printf("Decoded and emitted %zu synthetic COP1 instructions\n", cases.size());
    return output ? 0 : 5;
}
