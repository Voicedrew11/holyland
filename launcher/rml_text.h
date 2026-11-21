// Text put into the launcher's pages (window.h) as RML
#pragma once

#include <string>
#include <string_view>

namespace launcher {

// "a < b" as "a &lt; b": text made safe to put in RML (&, <, > and ")
std::string rml_escaped(std::string_view text);

} // namespace launcher
