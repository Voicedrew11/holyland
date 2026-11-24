// A zip archive made in memory (zip_archive.hpp). The format: PKWARE's APPNOTE.TXT, sections 4.3.7 (a local
// header), 4.3.12 (the central directory) and 4.3.16 (its end).
#include "zip_archive.hpp"

#include <cstdlib>
#include <cstring>
#include <memory>

// libchdr's miniz (cmake/libchdr.cmake builds its deflate too), declared as it is built
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#include "deps/miniz-3.1.2/miniz.h"

namespace report {
namespace {

constexpr std::uint32_t kLocalHeader = 0x04034b50;
constexpr std::uint32_t kCentralHeader = 0x02014b50;
constexpr std::uint32_t kDirectoryEnd = 0x06054b50;
constexpr std::uint16_t kVersion = 20;     // 2.0: deflate, folders
constexpr std::uint16_t kUtf8Names = 0x0800; // general purpose bit 11: the names are UTF-8
constexpr std::uint16_t kStored = 0, kDeflated = 8;
constexpr std::uint64_t kMost32 = 0xFFFFFFFFu;

struct Written {
    std::uint16_t method;
    std::uint32_t crc, packed_size, size, offset;
};

void put16(std::vector<std::uint8_t> &out, std::uint32_t v) {
    out.push_back(std::uint8_t(v));
    out.push_back(std::uint8_t(v >> 8));
}

void put32(std::vector<std::uint8_t> &out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

// MS-DOS's date and time (the format's own): from 1980, to two seconds
std::uint32_t dos_date_time(std::time_t made) {
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &made) != 0)
        return 0x00210000; // 1980-01-01
#else
    if (localtime_r(&made, &local) == nullptr)
        return 0x00210000;
#endif
    if (local.tm_year < 80)
        return 0x00210000;
    std::uint32_t date = std::uint32_t((local.tm_year - 80) << 9 | (local.tm_mon + 1) << 5 | local.tm_mday);
    std::uint32_t time = std::uint32_t(local.tm_hour << 11 | local.tm_min << 5 | local.tm_sec / 2);
    return date << 16 | time;
}

struct FreeMiniz {
    void operator()(void *p) const { mz_free(p); }
};

// The entry's data deflated (raw, as zip wants it), or empty when deflating gives nothing smaller
std::vector<std::uint8_t> deflated(const std::vector<std::uint8_t> &data) {
    if (data.empty())
        return {};
    std::size_t size = 0;
    const int flags = int(tdefl_create_comp_flags_from_zip_params(6, -MZ_DEFAULT_WINDOW_BITS, MZ_DEFAULT_STRATEGY));
    std::unique_ptr<void, FreeMiniz> packed(tdefl_compress_mem_to_heap(data.data(), data.size(), &size, flags));
    if (!packed || size >= data.size())
        return {};
    const auto *bytes = static_cast<const std::uint8_t *>(packed.get());
    return {bytes, bytes + size};
}

void put_local_header(std::vector<std::uint8_t> &out, const ZipEntry &entry, const Written &w, std::uint32_t when) {
    put32(out, kLocalHeader);
    put16(out, kVersion);
    put16(out, kUtf8Names);
    put16(out, w.method);
    put32(out, when);
    put32(out, w.crc);
    put32(out, w.packed_size);
    put32(out, w.size);
    put16(out, std::uint32_t(entry.name.size()));
    put16(out, 0); // no extra field
    out.insert(out.end(), entry.name.begin(), entry.name.end());
}

void put_central_header(std::vector<std::uint8_t> &out, const ZipEntry &entry, const Written &w, std::uint32_t when) {
    put32(out, kCentralHeader);
    put16(out, kVersion); // made by: MS-DOS's attributes, version 2.0
    put16(out, kVersion);
    put16(out, kUtf8Names);
    put16(out, w.method);
    put32(out, when);
    put32(out, w.crc);
    put32(out, w.packed_size);
    put32(out, w.size);
    put16(out, std::uint32_t(entry.name.size()));
    put16(out, 0); // extra field
    put16(out, 0); // comment
    put16(out, 0); // disk
    put16(out, 0); // internal attributes
    put32(out, 0); // external attributes
    put32(out, w.offset);
    out.insert(out.end(), entry.name.begin(), entry.name.end());
}

std::uint32_t get16(std::span<const std::uint8_t> in, std::size_t at) {
    return std::uint32_t(in[at]) | std::uint32_t(in[at + 1]) << 8;
}

std::uint32_t get32(std::span<const std::uint8_t> in, std::size_t at) {
    return get16(in, at) | get16(in, at + 2) << 16;
}

// One entry read back from its central header at `at`: nullopt when anything in it is out of place
std::optional<ZipEntry> read_entry(std::span<const std::uint8_t> in, std::size_t at, std::size_t *next) {
    if (at + 46 > in.size() || get32(in, at) != kCentralHeader)
        return std::nullopt;
    const std::uint32_t method = get16(in, at + 10), crc = get32(in, at + 16);
    const std::uint32_t packed_size = get32(in, at + 20), size = get32(in, at + 24);
    const std::size_t name_size = get16(in, at + 28), skip = get16(in, at + 30) + get16(in, at + 32);
    const std::size_t local = get32(in, at + 42);
    if (at + 46 + name_size + skip > in.size())
        return std::nullopt;
    ZipEntry entry;
    entry.name.assign(reinterpret_cast<const char *>(in.data() + at + 46), name_size);
    *next = at + 46 + name_size + skip;
    if (local + 30 > in.size() || get32(in, local) != kLocalHeader)
        return std::nullopt;
    const std::size_t data_at = local + 30 + get16(in, local + 26) + get16(in, local + 28);
    if (data_at + packed_size > in.size())
        return std::nullopt;
    const std::uint8_t *data = in.data() + data_at;
    if (method == kStored) {
        if (packed_size != size)
            return std::nullopt;
        entry.data.assign(data, data + size);
    } else if (method == kDeflated) {
        entry.data.resize(size);
        if (size != 0 && tinfl_decompress_mem_to_mem(entry.data.data(), size, data, packed_size, 0) != size)
            return std::nullopt;
    } else {
        return std::nullopt;
    }
    if (std::uint32_t(mz_crc32(MZ_CRC32_INIT, entry.data.data(), entry.data.size())) != crc)
        return std::nullopt;
    return entry;
}

} // namespace

std::optional<std::vector<std::uint8_t>> zip_archive(std::span<const ZipEntry> entries, std::time_t made) {
    if (entries.size() > 0xFFFF)
        return std::nullopt;
    const std::uint32_t when = dos_date_time(made);
    std::vector<std::uint8_t> out;
    std::vector<Written> written;
    written.reserve(entries.size());
    for (const ZipEntry &entry : entries) {
        if (entry.name.size() > 0xFFFF || entry.data.size() > kMost32 || out.size() > kMost32)
            return std::nullopt;
        std::vector<std::uint8_t> packed = deflated(entry.data);
        const bool stored = packed.empty();
        const std::vector<std::uint8_t> &body = stored ? entry.data : packed;
        Written w{stored ? kStored : kDeflated,
                  std::uint32_t(mz_crc32(MZ_CRC32_INIT, entry.data.data(), entry.data.size())),
                  std::uint32_t(body.size()), std::uint32_t(entry.data.size()), std::uint32_t(out.size())};
        put_local_header(out, entry, w, when);
        out.insert(out.end(), body.begin(), body.end());
        written.push_back(w);
    }
    const std::uint64_t directory_at = out.size();
    for (std::size_t k = 0; k < entries.size(); k++)
        put_central_header(out, entries[k], written[k], when);
    const std::uint64_t directory_size = out.size() - directory_at;
    if (directory_at > kMost32 || directory_size > kMost32)
        return std::nullopt;
    put32(out, kDirectoryEnd);
    put16(out, 0); // this disk
    put16(out, 0); // the directory's disk
    put16(out, std::uint32_t(entries.size()));
    put16(out, std::uint32_t(entries.size()));
    put32(out, std::uint32_t(directory_size));
    put32(out, std::uint32_t(directory_at));
    put16(out, 0); // no comment
    return out;
}

std::optional<std::vector<ZipEntry>> zip_entries(std::span<const std::uint8_t> archive) {
    if (archive.size() < 22)
        return std::nullopt;
    const std::size_t end = archive.size() - 22; // (zip_archive writes no comment)
    if (get32(archive, end) != kDirectoryEnd)
        return std::nullopt;
    const std::size_t count = get16(archive, end + 10);
    std::size_t at = get32(archive, end + 16);
    std::vector<ZipEntry> entries;
    for (std::size_t k = 0; k < count; k++) {
        std::optional<ZipEntry> entry = read_entry(archive, at, &at);
        if (!entry)
            return std::nullopt;
        entries.push_back(std::move(*entry));
    }
    return entries;
}

} // namespace report
