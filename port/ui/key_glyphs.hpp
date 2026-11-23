// The keyboard's keys and the mouse's buttons as the player menu shows them: their pictures (key_pictures.h, as the
// game's menus draw them with the Keyboard glyphs), as images in the menu's RML. The image is named by the key
// ("keycap/<the key's name in hex>.tex") and drawn when RmlUi first loads it.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "ui_frame.hpp"

namespace ui {

// The key or mouse button (SDL's name, port_controls_binding_label's: "Return", "Q", "Mouse left"; "Mouse" the mouse
// alone) as RML: <keycap><img src="keycap/..."/></keycap>. kf4.rcss sizes it.
std::string key_glyph_rml(std::string_view key_name);

// Whether a source is one of these images ("keycap/...")
bool is_key_glyph_source(std::string_view source);

// The image a source names, premultiplied RGBA; nullopt when the source is not one
std::optional<TextureUpload> load_key_glyph(std::string_view source);

} // namespace ui
