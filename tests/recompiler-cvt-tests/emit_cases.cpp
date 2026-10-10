// SPDX-License-Identifier: GPL-3.0-or-later
#include "ps2recomp/code_generator.h"
#include "ps2recomp/r5900_decoder.h"
#include "ps2recomp/Translators/fpu_translator.h"
#include "ps2recomp/types.h"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

constexpr uint32_t instruction(unsigned format, unsigned fd, unsigned fs,
    unsigned ft, unsigned function) {
    return 0x44000000u | (format << 21u) | (ft << 16u) |
        (fs << 11u) | (fd << 6u) | function;
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const std::vector<ps2recomp::Symbol> symbols;
    const std::vector<ps2recomp::Section> sections;
    ps2recomp::CodeGenerator generator(symbols, sections);
    ps2recomp::FpuTranslator translator(generator);
    ps2recomp::R5900Decoder decoder;
    std::ofstream output(argv[1]);
    output << "// Source-only synthetic opcodes through the actual decoder and translator.\n"
              "#pragma once\n#include \"ps2_runtime_macros.h\"\n";
    auto emit = [&](unsigned format, unsigned fd, unsigned fs, unsigned ft,
        unsigned function) {
        const auto decoded = decoder.decodeInstruction(0x100000u,
            instruction(format, fd, fs, ft, function), false);
        if (decoded.rs != format || decoded.sa != fd || decoded.rd != fs ||
            decoded.rt != ft || decoded.function != function) throw "decoded fields";
        const std::string code = translator.translate(decoded);
        if (code.empty() || code.find("Unhandled") != std::string::npos) throw "translation";
        output << code << '\n';
    };
    // Every possible FPR destination/source alias, plus distinct unary sources
    // with a deliberately nonzero Ft field that CVT.W.S must ignore.
    for (unsigned index = 0; index < 35; ++index) {
        const unsigned fd = index < 32 ? index : index == 32 ? 21 : index == 33 ? 0 : 31;
        const unsigned fs = index < 32 ? index : index == 32 ? 7 : index == 33 ? 31 : 0;
        const unsigned ft = (fs + 13) & 31;
        output << "inline void cvt_" << index << "(R5900Context* ctx) {\n";
        emit(16, fd, fs, ft, 0x24);
        output << "}\n";
    }
    output << "struct GeneratedCase { void (*function)(R5900Context*); unsigned fd, fs, ft; };\n"
              "inline constexpr GeneratedCase cases[] = {\n";
    for (unsigned index = 0; index < 35; ++index) {
        const unsigned fd = index < 32 ? index : index == 32 ? 21 : index == 33 ? 0 : 31;
        const unsigned fs = index < 32 ? index : index == 32 ? 7 : index == 33 ? 31 : 0;
        output << "{cvt_" << index << ',' << fd << ',' << fs << ',' << ((fs + 13) & 31) << "},\n";
    }
    output << "};\ninline void wrap_angle(R5900Context* ctx) {\n";
    // f12 = angle, f20 = 2pi, f21 = pi. This conventional range-reduction
    // sequence requires truncation after (angle+pi)/2pi.
    emit(16, 12, 12, 21, 0x00); // ADD.S
    emit(16, 1, 12, 20, 0x03);  // DIV.S
    emit(16, 0, 1, 0, 0x24);   // CVT.W.S
    emit(20, 0, 0, 0, 0x20);   // CVT.S.W
    emit(16, 0, 0, 20, 0x02);  // MUL.S
    emit(16, 12, 12, 0, 0x01); // SUB.S
    emit(16, 12, 12, 21, 0x01);// SUB.S
    output << "}\n";
    std::printf("Emitted 35 CVT aliases/source cases and a synthetic range-reduction sequence\n");
    return output ? 0 : 3;
}
