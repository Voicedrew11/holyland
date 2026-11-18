// The disc check (disc_check.h)
#include "disc_check.h"

#include <cctype>
#include <cstdio>
#include <memory>
#include <optional>
#include <system_error>

#include "launcher_files.h"

extern "C" {
#include "port/disc_image.h"
#include "port/iso9660.h"
}

namespace launcher {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kBoot = "SLUS_203.18"; // the USA release, the decomp's (decomp/config/sha1sums.txt)
constexpr std::uint32_t kMaxSystemCnf = 4096;

std::string upper(std::string_view text) {
    std::string out(text);
    for (char &c : out)
        c = (char)std::toupper((unsigned char)c);
    return out;
}

// what "BOOT2 = cdrom0:\SLUS_203.18;1" boots: "SLUS_203.18"
std::string boot_of(std::string_view system_cnf) {
    std::string text = upper(system_cnf);
    std::size_t at = text.find("BOOT2");
    if (at == std::string::npos)
        return {};
    at = text.find("CDROM0:", at);
    if (at == std::string::npos)
        return {};
    at += 7;
    while (at < text.size() && (text[at] == '\\' || text[at] == '/'))
        at++;
    std::size_t end = at;
    while (end < text.size() && text[end] != ';' && !std::isspace((unsigned char)text[end]))
        end++;
    return text.substr(at, end - at);
}

struct FileCloser {
    void operator()(FILE *f) const { std::fclose(f); }
};

struct ImageCloser {
    void operator()(DiscImage *image) const { disc_image_close(image); }
};

struct IsoListing {
    std::uint32_t system_cnf_lsn = 0;
    std::uint32_t system_cnf_size = 0;
    bool has_system_cnf = false;
    bool has_kf4_dat = false;
};

void note_file(void *context, const char *path, std::uint32_t lsn, std::uint32_t size) {
    auto *listing = static_cast<IsoListing *>(context);
    std::string name = upper(path);
    if (name == "\\SYSTEM.CNF") {
        listing->has_system_cnf = true;
        listing->system_cnf_lsn = lsn;
        listing->system_cnf_size = size;
    } else if (name == "\\DATA\\KF4.DAT") {
        listing->has_kf4_dat = true;
    }
}

DiscCheck check_image(const fs::path &path) {
    FILE *file = open_file(path, "rb");
    if (file == nullptr)
        return {DiscVerdict::unreadable, "The disc image cannot be opened.", {}};
    DiscImage *opened = nullptr;
    DiscImageResult result = disc_image_open(file, &opened); // (owns the file from here)
    if (result != DISC_IMAGE_OK) {
        std::string why = disc_image_result_text(result);
        why[0] = (char)std::toupper((unsigned char)why[0]);
        return {DiscVerdict::unreadable, why + ".", {}};
    }
    std::unique_ptr<DiscImage, ImageCloser> image(opened);
    IsoListing listing;
    if (iso9660_list(image.get(), nullptr, note_file, &listing) != ISO9660_OK)
        return {DiscVerdict::unreadable,
                "This file is not a disc image the game can read. Use an ISO (.iso) or CHD (.chd) image of the disc.",
                {}};
    std::string system_cnf;
    if (listing.has_system_cnf && listing.system_cnf_size <= kMaxSystemCnf) {
        std::uint32_t sectors = (listing.system_cnf_size + ISO9660_SECTOR_SIZE - 1) / ISO9660_SECTOR_SIZE;
        std::string buffer((std::size_t)(sectors ? sectors : 1) * ISO9660_SECTOR_SIZE, '\0');
        if (disc_image_read(image.get(), listing.system_cnf_lsn, sectors, buffer.data()))
            system_cnf = buffer.substr(0, listing.system_cnf_size);
    }
    return judge_disc(system_cnf, listing.has_kf4_dat);
}

// a file in `dir` by name in any case
std::optional<fs::path> find_any_case(const fs::path &dir, std::string_view name) {
    std::error_code error;
    for (fs::directory_iterator it(dir, error), end; !error && it != end; it.increment(error)) {
        if (upper(it->path().filename().string()) == upper(name))
            return it->path();
    }
    return std::nullopt;
}

DiscCheck check_folder(const fs::path &dir) {
    std::string system_cnf;
    if (auto file = find_any_case(dir, "SYSTEM.CNF")) {
        std::error_code error;
        if (fs::file_size(*file, error) <= kMaxSystemCnf && !error) {
            std::unique_ptr<FILE, FileCloser> f(open_file(*file, "rb"));
            char buffer[kMaxSystemCnf];
            if (f)
                system_cnf.assign(buffer, std::fread(buffer, 1, sizeof(buffer), f.get()));
        }
    }
    bool has_kf4_dat = false;
    if (auto data = find_any_case(dir, "DATA"))
        has_kf4_dat = find_any_case(*data, "KF4.DAT").has_value();
    return judge_disc(system_cnf, has_kf4_dat);
}

} // namespace

fs::path disc_meant(const fs::path &path) {
    std::error_code error;
    if (fs::is_regular_file(path, error) && upper(path.filename().string()) == "SYSTEM.CNF")
        return path.parent_path();
    return path;
}

DiscCheck judge_disc(std::string_view system_cnf, bool has_kf4_dat) {
    std::string boot = boot_of(system_cnf);
    if (system_cnf.empty() || !has_kf4_dat)
        return {DiscVerdict::not_kf4, "This is not King's Field IV's disc: it has no SYSTEM.CNF or DATA\\KF4.DAT.", boot};
    if (boot != kBoot)
        return {DiscVerdict::other_release,
                "This is King's Field IV, but not the USA release (SLUS-20318, \"King's Field: The Ancient City\"), "
                "the one the PC port is built from. This disc boots " +
                    (boot.empty() ? std::string("an unknown program") : boot) + ".",
                boot};
    return {DiscVerdict::ok, {}, boot};
}

DiscCheck check_disc(const fs::path &given) {
    fs::path path = disc_meant(given);
    std::error_code error;
    if (path.empty() || !fs::exists(path, error))
        return {DiscVerdict::missing, "There is no disc at " + path.string() + ".", {}};
    if (fs::is_directory(path, error))
        return check_folder(path);
    return check_image(path);
}

} // namespace launcher
