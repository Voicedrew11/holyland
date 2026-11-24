// A report's files on their way into its zip (report_bundle.hpp)
#include "report_bundle.hpp"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;

namespace report {
namespace {

constexpr std::uintmax_t kSaveFileBytes = 8u << 20; // a save file bigger than this is not a save
constexpr std::uintmax_t kSavesBytes = 32u << 20;

// A path's name as UTF-8, under C++17 (the launcher) and C++20 (the game, where u8string is std::u8string)
std::string utf8_name(const fs::path &path) {
    const auto name = path.filename().u8string();
    return std::string(name.begin(), name.end());
}

std::vector<std::uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

bool write_whole(const fs::path &path, const std::vector<std::uint8_t> &bytes) {
    fs::path part = path;
    part += ".tmp";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        if (!out.flush())
            return false;
    }
    std::error_code error;
    fs::rename(part, path, error);
    return !error;
}

} // namespace

void ReportBundle::add_text(const std::string &name, std::string_view text) {
    entries_.push_back({name, bytes_of(redacted(text))});
}

bool ReportBundle::add_file(const std::string &name, const fs::path &path) {
    std::optional<std::string> body = read_text(path);
    if (!body)
        return false;
    entries_.push_back({name, bytes_of(*body)});
    return true;
}

bool ReportBundle::add_log(const std::string &name, const fs::path &path, std::size_t head_lines,
                           std::size_t tail_bytes) {
    std::optional<std::string> log = read_text(path);
    if (!log)
        return false;
    add_text(name, head_and_tail(*log, head_lines, tail_bytes));
    return true;
}

void ReportBundle::add_save(const std::string &name, const fs::path &path) {
    std::error_code error;
    const std::uintmax_t size = fs::file_size(path, error);
    if (error || size > kSaveFileBytes || save_bytes_ + size > kSavesBytes)
        return;
    if (std::optional<std::string> body = read_text(path)) {
        save_bytes_ += body->size();
        entries_.push_back({name, bytes_of(*body)});
    }
}

void ReportBundle::add_save_folder(const std::string &prefix, const fs::path &folder) {
    std::error_code error;
    for (const fs::directory_entry &entry : fs::directory_iterator(folder, error)) {
        if (entry.is_regular_file(error))
            add_save(prefix + utf8_name(entry.path()), entry.path());
    }
}

std::optional<fs::path> ReportBundle::write(const fs::path &folder, const std::string &stem) const {
    const std::optional<std::vector<std::uint8_t>> archive = zip_archive(entries_, std::time(nullptr));
    std::error_code error;
    fs::create_directories(folder, error);
    const fs::path zip = folder / (stem + ".zip");
    if (!archive || !write_whole(zip, *archive))
        return std::nullopt;
    return zip;
}

std::string report_timestamp() {
    std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[32];
    std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
    return text;
}

std::optional<std::string> read_text(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

bool written_within(const fs::path &path, std::chrono::hours age) {
    std::error_code error;
    const fs::file_time_type written = fs::last_write_time(path, error);
    return !error && fs::file_time_type::clock::now() - written < age;
}

bool keep_link_instead(const fs::path &zip, const std::string &url) {
    const char *no_open = std::getenv("KF4_REPORT_NO_OPEN");
    if (no_open == nullptr || no_open[0] != '1')
        return false;
    fs::path file = zip;
    file += ".url.txt";
    std::ofstream(file) << url << "\n";
    return true;
}

} // namespace report
