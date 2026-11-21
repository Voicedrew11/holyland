// Text put into the launcher's pages as RML (rml_text.h)
#include "rml_text.h"

namespace launcher {

std::string rml_escaped(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += "&quot;";
            break;
        default:
            out += c;
        }
    }
    return out;
}

} // namespace launcher
