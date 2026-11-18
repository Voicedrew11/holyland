// Ninja's progress lines (build_log.h)
#include "build_log.h"

namespace launcher {

namespace {

// a number at `at` up to `end`; moves `at` past it
std::optional<int> read_number(std::string_view line, std::size_t &at, char end) {
    int value = 0;
    std::size_t start = at;
    while (at < line.size() && line[at] >= '0' && line[at] <= '9') {
        if (value > 10000000)
            return std::nullopt;
        value = value * 10 + (line[at] - '0');
        at++;
    }
    if (at == start || at >= line.size() || line[at] != end)
        return std::nullopt;
    at++;
    return value;
}

} // namespace

std::optional<NinjaStep> ninja_step(std::string_view line) {
    if (line.empty() || line[0] != '[')
        return std::nullopt;
    std::size_t at = 1;
    auto done = read_number(line, at, '/');
    if (!done)
        return std::nullopt;
    auto total = read_number(line, at, ']');
    if (!total || *total == 0 || *done > *total)
        return std::nullopt;
    return NinjaStep{*done, *total};
}

} // namespace launcher
