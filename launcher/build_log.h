// What the build's output says about its progress: Ninja's "[12/3456] Building C object ..." lines.
#pragma once

#include <optional>
#include <string_view>

namespace launcher {

struct NinjaStep {
    int done;
    int total;
};

// the step a line of Ninja's output reports, if it is one
std::optional<NinjaStep> ninja_step(std::string_view line);

} // namespace launcher
