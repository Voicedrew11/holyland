// The crash report (crash_report.h)
#include "crash_report.h"

#include "pc_facts.hpp"
#include "report_bundle.hpp"
#include "report_text.hpp"

#include "port/update_notice.h" // UPDATE_REPOSITORY

namespace launcher {
namespace {

constexpr std::size_t kLinkBytes = 8000;
constexpr std::size_t kLogHeadLines = 300;
constexpr std::size_t kLogTailBytes = 3u << 20;
constexpr std::size_t kPreviousLogTailBytes = 1u << 20;
constexpr std::size_t kBuildLogTailBytes = 256u << 10;
constexpr std::size_t kLogLinesInLink = 25; // the log's end in the form: the crash's own lines

constexpr const char *kReadme =
    "A Holyland crash report (made by the launcher's Report the problem, when the game stopped).\n\n"
    "crash.txt         how the game ended, the launcher's version, the game's command line\n"
    "system.txt        the system, the CPU, memory; the GPU is in the log's first lines\n"
    "environment.txt   the game's command line and the variables that change the game, SDL or the GPU's driver\n"
    "settings.json     the settings file\n"
    "logs/             the run that stopped, the run before it, the launcher's build log\n"
    "saves/            the save points' saves and the quick saves\n\n"
    "The home folder, the user's and the computer's names and e-mail addresses are taken out of the texts.\n";

std::string version_text(const std::string &version) {
    return version.empty() ? "a developer's build" : version;
}

// The log's last lines, for the form's Details
std::string last_lines(const std::string &log, std::size_t lines) {
    std::size_t at = log.size();
    if (at > 0 && log[at - 1] == '\n')
        at--;
    for (std::size_t k = 0; k < lines; k++) {
        const std::size_t newline = at > 0 ? log.rfind('\n', at - 1) : std::string::npos;
        if (newline == std::string::npos)
            return log;
        at = newline;
    }
    return log.substr(at + 1);
}

} // namespace

std::optional<CrashReport> make_crash_report(const DataPaths &paths, const CrashFacts &facts) {
    report::ReportBundle bundle(report::private_words());
    bundle.add_text("README.txt", kReadme);
    const std::string crash = "The game stopped: " + facts.ended + "\nLauncher: " +
                              version_text(facts.launcher_version) + "\nGame's command line: " +
                              facts.game_command_line + "\n";
    bundle.add_text("crash.txt", crash);
    const std::string system = report::os_facts() + report::cpu_facts() + report::memory_facts() +
                               report::disk_facts(paths.data);
    bundle.add_text("system.txt", system);
    bundle.add_text("environment.txt", report::environment_facts(facts.game_command_line));
    if (auto settings = report::read_text(paths.data / "settings.json"))
        bundle.add_text("settings.json", *settings);
    bundle.add_log("logs/game.log", paths.game_log(), kLogHeadLines, kLogTailBytes);
    if (report::written_within(paths.game_prev_log(), std::chrono::hours(24)))
        bundle.add_log("logs/game.prev.log", paths.game_prev_log(), kLogHeadLines, kPreviousLogTailBytes);
    bundle.add_log("logs/build.log", paths.build_log(), 40, kBuildLogTailBytes);
    bundle.add_save_folder("saves/", paths.data / "saves");
    bundle.add_save_folder("saves/quicksaves/", paths.data / "quicksaves");

    const std::string stamp = report::report_timestamp();
    const std::optional<std::filesystem::path> zip = bundle.write(paths.data / "reports", "holyland-crash-" + stamp);
    if (!zip)
        return std::nullopt;
    std::string details = "Report file: " + zip->filename().u8string() + "\n\nThe game stopped: " + facts.ended +
                          "\nLauncher: " + version_text(facts.launcher_version) + "\n" + system;
    if (auto log = report::read_text(paths.game_log()))
        details += "\nThe log's end:\n" + last_lines(*log, kLogLinesInLink);
    const std::string title = "The game stopped: " + facts.ended;
    const std::string url = report::issue_url(
        {UPDATE_REPOSITORY, bundle.redacted(title), "", "", bundle.redacted(details)}, kLinkBytes);
    return CrashReport{*zip, url};
}

} // namespace launcher
