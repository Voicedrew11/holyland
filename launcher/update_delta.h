// Delta updates (docs/port/LAUNCHER.md, "Deltas"): beside its whole files, a release carries for each of the last
// few releases a small file that makes its whole file from what that release installed. Its name is the whole file's
// with ".from-<version>.delta" after it, and it is listed in the signed SHA256SUMS like every file. Two kinds, made by
// packaging/make_deltas.sh:
//
// - a file patch (the AppImage): `zstd --patch-from=<old file> <new file>`, one zstd frame that needs the old file
//   as its reference;
// - a tree delta (the Windows zip): a zip of the files that are new or changed, and kDeltaListing at its top, every
//   file of the new release's zip as "<sha256>  <path>" a line (sha256sum's lines, '/' between folders).
//
// Whatever a delta makes is checked before it is used: the patched file against the whole file's SHA-256 in
// SHA256SUMS, the tree file by file against its listing. Any failure and the whole file is downloaded instead.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace launcher {

namespace fs = std::filesystem;

// the delta's name from its release's whole file and the version it updates from:
// ("Holyland-0.3.0-x86_64.AppImage", "0.2.0") -> "Holyland-0.3.0-x86_64.AppImage.from-0.2.0.delta"
std::string delta_name(std::string_view whole_name, std::string_view from_version);

// the most an update's file (whole, patched or a delta) may be: the AppImage is ~170 MB
constexpr std::uint64_t kUpdateMaxBytes = 1024ull * 1024 * 1024;

constexpr const char *kDeltaListing = "delta-files.txt";

// The file patch `patch` applied to `old_file`, written to `out` (at most `max_bytes`), whose SHA-256 must be
// `whole_sha256`. "" when `out` is that file; else why not, with `out` removed.
std::string apply_file_patch(const fs::path &old_file, const fs::path &patch, const fs::path &out,
                             std::string_view whole_sha256, std::uint64_t max_bytes);

// A tree delta unpacked into `staged` made into the new release's whole tree: each listed file not in `staged` is
// linked (or copied) from `installed`, every file is checked against the listing, nothing unlisted may be there, and
// the listing is removed. "" when `staged` holds exactly the listed files; else why not (`staged` is then the
// caller's to remove). A listed path that is absolute or climbs out ("..") is refused.
std::string complete_tree(const fs::path &staged, const fs::path &installed);

} // namespace launcher
