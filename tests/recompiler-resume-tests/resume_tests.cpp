#include "ps2recomp/ps2_recompiler.h"
#include "ps2recomp/types.h"
#include <elfio/elfio.hpp>

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <set>
#include <string>
#include <vector>

#ifndef EXPECT_ORIGINAL_MISSING_ALIASES
#define EXPECT_ORIGINAL_MISSING_ALIASES 0
#endif
using namespace ps2recomp;
namespace fs = std::filesystem;
namespace {
unsigned checks = 0u, failures = 0u;
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::fprintf(stderr, "FAIL: %s\n", message); }
}
std::string read(const fs::path& path) {
    std::ifstream file(path);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
bool writeElf(const fs::path& path) {
    ELFIO::elfio writer;
    writer.create(ELFIO::ELFCLASS32, ELFIO::ELFDATA2LSB);
    writer.set_os_abi(ELFIO::ELFOSABI_NONE);
    writer.set_type(ELFIO::ET_EXEC);
    writer.set_machine(ELFIO::EM_MIPS);
    writer.set_entry(0x100000u);
    auto* text = writer.sections.add(".text");
    text->set_type(ELFIO::SHT_PROGBITS);
    text->set_flags(ELFIO::SHF_ALLOC | ELFIO::SHF_EXECINSTR);
    text->set_addr_align(4u);
    text->set_address(0x100000u);
    std::array<uint32_t, 32> words{};
    words[0] = 0x03e00008u; // Known bootstrap JR RA.
    // This standalone routine has no symbol and is not called by bootstrap.
    // A configured executable entry hint must synthesize its decoded wrapper.
    words[4] = 0x27bdfff0u; // 100010: ADDIU SP,-16
    words[5] = 0xafbf0000u; // 100014: SW RA,0(SP)
    words[6] = 0x0c040018u; // 100018: JAL 100060; resume 100020
    words[8] = 0x24190060u; // 100020: ADDIU T9,0,60
    words[9] = 0x0320f809u; // 100024: JALR T9; resume 10002C
    words[11] = 0x0000000cu;// 10002C: SYSCALL; resume 100030
    words[12] = 0x0c04001cu;// 100030: JAL 100070; resume 100038
    words[14] = 0x8fbf0000u;// 100038: LW RA,0(SP)
    words[15] = 0x27bd0010u;// 10003C: ADDIU SP,16
    words[16] = 0x03e00008u;// 100040: JR RA
    words[20] = 0x08040019u;// 100050: J 100064; external mid-function target.
    words[24] = 0x03e00008u;// Known callee 100060.
    words[28] = 0x03e00008u;// Known callee 100070.
    text->set_data(reinterpret_cast<const char*>(words.data()), sizeof(words));
    auto* strings = writer.sections.add(".strtab");
    strings->set_type(ELFIO::SHT_STRTAB);
    strings->set_addr_align(1u);
    auto* symbols = writer.sections.add(".symtab");
    symbols->set_type(ELFIO::SHT_SYMTAB);
    symbols->set_info(1u);
    symbols->set_link(strings->get_index());
    symbols->set_addr_align(4u);
    symbols->set_entry_size(writer.get_default_entry_size(ELFIO::SHT_SYMTAB));
    ELFIO::symbol_section_accessor accessor(writer, symbols);
    ELFIO::string_section_accessor names(strings);
    accessor.add_symbol(names, "", 0, 0, ELFIO::STB_LOCAL, ELFIO::STT_NOTYPE, 0, ELFIO::SHN_UNDEF);
    for (const auto& [name, address] : std::array<std::pair<const char*, uint32_t>, 3>{
             {{"bootstrap", 0x100000u}, {"callee_a", 0x100060u}, {"callee_b", 0x100070u}}})
        accessor.add_symbol(names, name, address, 8u, ELFIO::STB_GLOBAL, ELFIO::STT_FUNC, 0, text->get_index());
    auto* segment = writer.segments.add();
    segment->set_type(ELFIO::PT_LOAD);
    segment->set_flags(ELFIO::PF_R | ELFIO::PF_X);
    segment->set_align(0x1000u);
    segment->add_section_index(text->get_index(), text->get_addr_align());
    return writer.save(path.string());
}
std::map<uint32_t, std::string> registrations(const std::string& source) {
    std::map<uint32_t, std::string> result;
    const std::regex entry(R"(g_ps2RecompiledFunctionTable\[[0-9]+\] = ([a-zA-Z0-9_]+); // 0x([0-9a-f]+))");
    for (std::sregex_iterator i(source.begin(), source.end(), entry), end; i != end; ++i) {
        const uint32_t address = static_cast<uint32_t>(std::stoul((*i)[2].str(), nullptr, 16));
        check(result.emplace(address, (*i)[1].str()).second, "table contains no duplicate address assignments");
    }
    return result;
}
void pipeline(const fs::path& root, bool singleFile, unsigned workers) {
    fs::create_directories(root);
    const auto elf = root / "synthetic.elf";
    const auto config = root / "synthetic.toml";
    const auto output = root / "generated";
    check(writeElf(elf), "synthetic non-retail MIPS ELF writes");
    {
        std::ofstream file(config);
        file << "[general]\ninput = \"" << elf.generic_string() << "\"\noutput = \""
             << output.generic_string() << "\"\nentry_points = [\"omitted_callback@0x100010\"]\n"
             << "single_file_output = " << (singleFile ? "true" : "false") << '\n'
             << "output_worker_threads = " << workers << '\n';
        check(static_cast<bool>(file), "synthetic config writes");
    }
    PS2Recompiler recompiler(config.string());
    check(recompiler.initialize(), "actual ELF/config initialization succeeds");
    check(recompiler.recompile(), "actual discovery synthesizes/decodes omitted standalone entry");
    recompiler.generateOutput();
    const auto table = registrations(read(output / "register_functions.cpp"));
    const auto owner = table.find(0x100010u);
    check(owner != table.end(), "standalone entry is registered");
    if (owner == table.end()) return;
    check(owner->second.find("entry_") == 0u, "omitted routine really uses synthesized entry wrapper");
    std::string generated;
    for (const auto& file : fs::directory_iterator(output)) {
        if (file.path().extension() == ".cpp" && file.path().filename() != "register_functions.cpp")
            generated += read(file.path());
    }
    for (const uint32_t target : {0x100020u, 0x10002cu, 0x100030u, 0x100038u}) {
        char address[24]; std::snprintf(address, sizeof(address), "case 0x%xu:", target);
        check(generated.find(address) != std::string::npos, "JAL/JALR/syscall emitted continuation switch exists");
        const auto alias = table.find(target);
        if constexpr (EXPECT_ORIGINAL_MISSING_ALIASES)
            check(alias == table.end(), "original exclusion reproduces missing emitted continuation registration");
        else
            check(alias != table.end() && alias->second == owner->second, "every continuation aliases its exact emitted owner");
    }
    const std::regex resumeCase(R"(case 0x([0-9a-f]+)u: goto label_[0-9a-f]+;)");
    std::set<uint32_t> emittedCases;
    for (std::sregex_iterator i(generated.begin(), generated.end(), resumeCase), end; i != end; ++i)
        emittedCases.insert(static_cast<uint32_t>(std::stoul((*i)[1].str(), nullptr, 16)));
    check(emittedCases.size() >= 4u, "all emitted resume cases are audited, including indirect fallbacks");
    for (const uint32_t target : emittedCases) {
        const auto alias = table.find(target);
        if constexpr (EXPECT_ORIGINAL_MISSING_ALIASES)
            check(alias == table.end() || target == 0x100010u,
                  "old pipeline leaves emitted entry-wrapper switch cases unregistered");
        else
            check(alias != table.end() && alias->second == owner->second,
                  "every emitted resume switch case has the same dispatch-table owner");
    }
    check(table.at(0x100000u).find("bootstrap") != std::string::npos, "bootstrap registration is preserved");
    check(table.at(0x100060u).find("callee_a") != std::string::npos && table.at(0x100070u).find("callee_b") != std::string::npos,
          "explicit callee registrations are preserved");
    check(!table.contains(0x100064u),
          "entry-wrapper analysis does not promote unrelated external targets in normal owners");
    // Discovery stays source-only: no recursively synthesized continuation routines.
    check(generated.find("// Function: entry_100020") == std::string::npos &&
          generated.find("// Function: entry_10002c") == std::string::npos,
          "aliases do not broaden recursive wrapper discovery");
}
Function function(std::string name, uint32_t start, bool stub = false, bool skipped = false) {
    Function fn{}; fn.name = std::move(name); fn.start = start; fn.end = start + 0x40u;
    fn.isRecompiled = !stub && !skipped; fn.isStub = stub; fn.isSkipped = skipped; return fn;
}
void collisions() {
    const std::vector<Section> sections;
    CodeGenerator generator({}, sections);
    generator.setRenamedFunctions({{0x1000u, "owner"}, {0x1008u, "normal"}, {0x100cu, "stub"},
                                  {0x1010u, "syscall"}, {0x1014u, "library"}});
    generator.setResumeEntryTargets({{0x1000u, {0x1004u, 0x1008u, 0x100cu, 0x1010u, 0x1014u, 0x1004u}}});
    check(PS2Recompiler::resolveStubTarget("SleepThread") == StubTarget::Syscall, "test syscall is a real runtime binding");
    const auto table = registrations(generator.generateFunctionRegistration({
        function("entry_owner", 0x1000u), function("normal", 0x1008u),
        function("synthetic_stub", 0x100cu, true), function("SleepThread", 0x1010u, true),
        function("library", 0x1014u, false, true)}, {}));
    check(table.size() == 6u, "unique aliases add only one slot and duplicates are removed");
    check(table.at(0x1004u) == "owner", "unoccupied continuation address registers its owner");
    check(table.at(0x1008u) == "normal", "explicit recompiled start has priority over alias");
    check(table.at(0x100cu) == (EXPECT_ORIGINAL_MISSING_ALIASES ? "owner" : "stub"),
          "explicit stub owns its address; old control demonstrates shadowing");
    check(table.at(0x1010u) == (EXPECT_ORIGINAL_MISSING_ALIASES ? "owner" : "syscall"),
          "explicit kernel handler owns its address; old control demonstrates shadowing");
    check(table.at(0x1014u) == (EXPECT_ORIGINAL_MISSING_ALIASES ? "owner" : "library"),
          "explicit skipped/library function owns its address; old control demonstrates shadowing");
}
void overlappingOwners() {
    const std::vector<Section> sections;
    const std::vector<Function> functions{
        function("normal_wide", 0x2000u), function("normal_narrow", 0x2008u),
        function("entry_overlay", 0x200cu), function("entry_narrow", 0x2010u)};
    const std::map<uint32_t, std::vector<uint32_t>> targets{
        {0x2000u, {0x2018u, 0x2024u}}, {0x2008u, {0x2018u, 0x2028u}},
        {0x200cu, {0x2018u, 0x2020u, 0x2024u}}, {0x2010u, {0x2018u, 0x2020u}}};
    std::vector<std::map<uint32_t, std::string>> tables;
    for (const bool reverse : {false, true}) {
        CodeGenerator generator({}, sections);
        generator.setRenamedFunctions({{0x2000u, "normal_wide"}, {0x2008u, "normal_narrow"},
                                      {0x200cu, "entry_overlay"}, {0x2010u, "entry_narrow"}});
        std::unordered_map<uint32_t, std::vector<uint32_t>> owners;
        if (reverse) {
            for (auto it = targets.rbegin(); it != targets.rend(); ++it) owners.emplace(it->first, it->second);
        } else {
            for (const auto& [owner, aliases] : targets) owners.emplace(owner, aliases);
        }
        generator.setResumeEntryTargets(owners);
        tables.push_back(registrations(generator.generateFunctionRegistration(functions, {})));
        const auto& table = tables.back();
        check(table.at(0x2000u) == "normal_wide" && table.at(0x2008u) == "normal_narrow" &&
              table.at(0x200cu) == "entry_overlay" && table.at(0x2010u) == "entry_narrow",
              "every overlapping owner retains its explicit entry address");
        check(table.at(0x2028u) == "normal_narrow", "unique normal alias remains assigned to its owner");
        if constexpr (!EXPECT_ORIGINAL_MISSING_ALIASES) {
            check(table.at(0x2018u) == "normal_narrow", "most specific normal owner wins all overlapping entry aliases");
            check(table.at(0x2020u) == "entry_narrow", "most specific entry fallback wins only when no normal owner exists");
            check(table.at(0x2024u) == "normal_wide", "normal owner remains preferred to a more specific synthesized entry");
        }
    }
    if constexpr (!EXPECT_ORIGINAL_MISSING_ALIASES) {
        check(tables[0] == tables[1], "alias ownership is independent of unordered-map insertion order");
    } else {
        check(tables[0].at(0x2018u) != "normal_narrow" || tables[1].at(0x2018u) != "normal_narrow",
              "old unordered first-wins path reproduces overlapping alias ownership defect");
    }
}
}
int main() {
    const fs::path root = fs::current_path() / (EXPECT_ORIGINAL_MISSING_ALIASES ? "synthetic-control" : "synthetic-fixed");
    pipeline(root / "separate_serial", false, 1u);
    pipeline(root / "separate_parallel", false, 2u);
    pipeline(root / "combined_serial", true, 1u);
    collisions();
    overlappingOwners();
    std::printf("%s: %u checks, %u failures; four emitted continuations per synthetic entry\n",
                EXPECT_ORIGINAL_MISSING_ALIASES ? "Original missing-alias control" : "Resume alias regression", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
