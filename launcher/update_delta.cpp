// Delta updates (update_delta.h)
#include "update_delta.h"

#include <cstdio>
#include <memory>
#include <set>
#include <system_error>
#include <vector>

#include "build_key.h"
#include "launcher_files.h"
#include "platform.h"

extern "C" {
#include "zstd.h" // libchdr's copy (third_party/libchdr/deps/zstd-1.5.7)
#include "zstd_errors.h"
}

namespace launcher {

namespace {

constexpr int kWindowLogMax = 31; // the most zstd takes on a 64-bit system; --patch-from sizes it to both files
constexpr std::uint64_t kListingMaxBytes = 16ull * 1024 * 1024;

struct FileCloser {
    void operator()(std::FILE *f) const { std::fclose(f); }
};
using File = std::unique_ptr<std::FILE, FileCloser>;

struct DCtxFreer {
    void operator()(ZSTD_DCtx *d) const { ZSTD_freeDCtx(d); }
};

// (libchdr's zstd is built without its error texts: the number names it, zstd_errors.h)
std::string zstd_why(size_t code) {
    return "zstd error " + std::to_string((int)ZSTD_getErrorCode(code));
}

// the whole of `file` (at most `max_bytes`), or why not
std::string read_whole(const fs::path &file, std::uint64_t max_bytes, std::vector<char> &bytes) {
    std::error_code error;
    std::uint64_t size = fs::file_size(file, error);
    if (error)
        return "cannot see " + file.u8string() + ": " + error.message();
    if (size > max_bytes)
        return file.u8string() + " is larger than an update's file can be";
    File f(open_file(file, "rb"));
    if (!f)
        return "cannot open " + file.u8string();
    bytes.resize(size);
    if (size != 0 && std::fread(bytes.data(), 1, size, f.get()) != size)
        return "cannot read " + file.u8string();
    return {};
}

// the patch decoded into `out` against `reference`; its SHA-256 in hex, or why not in `why`
std::string decode_patch(std::FILE *patch, const std::vector<char> &reference, std::FILE *out, std::uint64_t max_bytes,
                         std::string &why) {
    std::unique_ptr<ZSTD_DCtx, DCtxFreer> dctx(ZSTD_createDCtx());
    if (!dctx) {
        why = "out of memory";
        return {};
    }
    size_t ret = ZSTD_DCtx_setParameter(dctx.get(), ZSTD_d_windowLogMax, kWindowLogMax);
    if (!ZSTD_isError(ret))
        ret = ZSTD_DCtx_refPrefix(dctx.get(), reference.data(), reference.size());
    if (ZSTD_isError(ret)) {
        why = "cannot start the patch: " + zstd_why(ret);
        return {};
    }
    std::vector<char> in_bytes(ZSTD_DStreamInSize()), out_bytes(ZSTD_DStreamOutSize());
    Sha256Hasher hasher;
    std::uint64_t written = 0;
    bool frame_done = false;
    size_t got;
    while ((got = std::fread(in_bytes.data(), 1, in_bytes.size(), patch)) > 0) {
        ZSTD_inBuffer input{in_bytes.data(), got, 0};
        while (input.pos < input.size) {
            if (frame_done) {
                // refPrefix covers the first frame alone: a second one is not a patch this launcher made
                why = "the patch has more after its end";
                return {};
            }
            ZSTD_outBuffer output{out_bytes.data(), out_bytes.size(), 0};
            ret = ZSTD_decompressStream(dctx.get(), &output, &input);
            if (ZSTD_isError(ret)) {
                why = "the patch does not fit this file: " + zstd_why(ret);
                return {};
            }
            written += output.pos;
            if (written > max_bytes) {
                why = "the patch makes a file larger than an update's file can be";
                return {};
            }
            hasher.add(std::string_view(out_bytes.data(), output.pos));
            if (output.pos != 0 && std::fwrite(out_bytes.data(), 1, output.pos, out) != output.pos) {
                why = "cannot write the patched file";
                return {};
            }
            frame_done = ret == 0;
        }
    }
    if (std::ferror(patch)) {
        why = "cannot read the patch";
        return {};
    }
    if (!frame_done) {
        why = "the patch ends early";
        return {};
    }
    return to_hex(hasher.finish());
}

// a listed path as one inside the tree, or nullopt when it is absolute, climbs out or is not plain
std::optional<fs::path> inside_path(const std::string &text) {
    if (text.empty() || text.find('\\') != std::string::npos || text.find(':') != std::string::npos ||
        text.find("//") != std::string::npos || text.back() == '/')
        return std::nullopt;
    fs::path path = fs::u8path(text);
    if (path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return std::nullopt;
    for (const fs::path &part : path) {
        if (part.empty() || part == "." || part == "..")
            return std::nullopt;
    }
    return path;
}

struct Listed {
    std::string sha256;
    fs::path path;
};

// kDeltaListing's lines, or why not in `why`
std::vector<Listed> parse_listing(const std::string &text, std::string &why) {
    std::vector<Listed> files;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = text.find('\n', at);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty())
            continue;
        // "<64 hex>  <path>" (sha256sum's text mode) or "<64 hex> *<path>" (its binary mode)
        bool hex = line.size() > 66 && line.find_first_not_of("0123456789abcdef") >= 64;
        if (!hex || line[64] != ' ' || (line[65] != ' ' && line[65] != '*')) {
            why = "the delta's list of files has a line that is not a checksum and a path";
            return {};
        }
        auto path = inside_path(line.substr(66));
        if (!path) {
            why = "the delta's list of files names a path outside its folder: " + line.substr(66);
            return {};
        }
        files.push_back(Listed{line.substr(0, 64), *path});
    }
    if (files.empty())
        why = "the delta's list of files is empty";
    return files;
}

// `from` at `to`, linked when the file system can, else copied; "" or why not
std::string link_or_copy(const fs::path &from, const fs::path &to) {
    std::error_code error;
    if (!fs::is_regular_file(from, error))
        return "this install has no " + from.u8string() + ", which the delta keeps";
    fs::create_directories(to.parent_path(), error);
    if (error)
        return "cannot make " + to.parent_path().u8string() + ": " + error.message();
    fs::create_hard_link(from, to, error);
    if (!error)
        return {};
    error.clear();
    fs::copy_file(from, to, error);
    if (error)
        return "cannot copy " + from.u8string() + ": " + error.message();
    return {};
}

} // namespace

std::string delta_name(std::string_view whole_name, std::string_view from_version) {
    return std::string(whole_name) + ".from-" + std::string(from_version) + ".delta";
}

std::string apply_file_patch(const fs::path &old_file, const fs::path &patch, const fs::path &out,
                             std::string_view whole_sha256, std::uint64_t max_bytes) {
    std::vector<char> reference;
    std::string why = read_whole(old_file, max_bytes, reference);
    if (!why.empty())
        return why;
    std::string sha256;
    {
        File patch_file(open_file(patch, "rb"));
        if (!patch_file)
            return "cannot open the patch " + patch.u8string();
        File out_file(open_file(out, "wb"));
        if (!out_file)
            return "cannot write " + out.u8string();
        sha256 = decode_patch(patch_file.get(), reference, out_file.get(), max_bytes, why);
        if (why.empty() && std::fflush(out_file.get()) != 0)
            why = "cannot write " + out.u8string();
        if (why.empty() && std::fclose(out_file.release()) != 0)
            why = "cannot write " + out.u8string();
    }
    if (why.empty() && sha256 != whole_sha256)
        why = "the patched file is not the release's file (its checksum differs)";
    if (!why.empty()) {
        std::error_code error;
        fs::remove(out, error);
    }
    return why;
}

std::string complete_tree(const fs::path &staged, const fs::path &installed) {
    std::error_code error;
    fs::path listing_file = staged / kDeltaListing;
    if (!fs::is_regular_file(listing_file, error) || fs::file_size(listing_file, error) > kListingMaxBytes)
        return std::string("the delta has no ") + kDeltaListing;
    auto listing = read_file(listing_file);
    if (!listing)
        return std::string("cannot read the delta's ") + kDeltaListing;
    std::string why;
    std::vector<Listed> files = parse_listing(*listing, why);
    if (!why.empty())
        return why;
    fs::remove(listing_file, error);
    if (error)
        return std::string("cannot remove the delta's ") + kDeltaListing + ": " + error.message();

    std::set<std::string> names;
    for (const Listed &file : files) {
        if (!names.insert(file.path.generic_u8string()).second)
            return "the delta lists " + file.path.generic_u8string() + " twice";
        fs::path at = staged / file.path;
        if (!fs::exists(at, error)) {
            why = link_or_copy(installed / file.path, at);
            if (!why.empty())
                return why;
        }
        if (!fs::is_regular_file(fs::symlink_status(at, error)))
            return file.path.generic_u8string() + " is not a plain file";
        if (file_sha256(at) != file.sha256)
            return file.path.generic_u8string() + " is not the release's file (its checksum differs)";
    }
    // nothing more than the listing
    for (fs::recursive_directory_iterator it(staged, error), end; !error && it != end; it.increment(error)) {
        fs::file_status status = it->symlink_status(error);
        if (error)
            break;
        if (fs::is_directory(status))
            continue;
        std::string name = it->path().lexically_relative(staged).generic_u8string();
        if (!fs::is_regular_file(status) || names.count(name) == 0)
            return "the delta holds " + name + ", which its list does not";
    }
    if (error)
        return "cannot look through " + staged.u8string() + ": " + error.message();
    return {};
}

Installation::Made Installation::made_from_delta(const fs::path &delta, std::string_view) const {
    std::error_code error;
    fs::remove(delta, error);
    return {{}, "this way of installing takes no deltas"};
}

} // namespace launcher
