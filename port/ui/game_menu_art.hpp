// The game's own menu art as the player menu's images: the textures of menu/menu.tmx on the player's disc (the
// pictures King's Field IV's menus are drawn from), decoded for RmlUi. Nothing of the game's is carried in the port:
// the art is read from the disc when the menu first draws it.
//
// A style sheet names one as "game-menu/<tbp>-<look>.tex": tbp is the texture's GS base pointer (TEX0's TBP0, as the
// game's menu parts in menu/*_para.dat name it), look is how the game draws it (MenuArtLook). See kf4.rcss's
// @spritesheet for the pieces the window uses.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ui {

// How the game draws a texture, which decides its premultiplied colours (the menu blends premultiplied)
enum class MenuArtLook {
    over,      // blended over by the texture's alpha (Menu_SetBlendMode 0)
    add,       // added, the texture's alpha unused (Menu_SetBlendMode 1, TEX0.TCC 0): the sheet's image-color scales it
    add_alpha, // added, scaled by the texture's alpha (Menu_SetBlendMode 1, TEX0.TCC 1)
};

// An image for RmlUi: premultiplied RGBA, rows top first
struct MenuArtImage {
    int32_t width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

// The texture of a menu.tmx file whose base pointer is `tbp`, in that look; nullopt when the file has no such texture
// or is not whole. Only the formats menu.tmx uses: 8 and 4 bits a pixel with a 32-bit CLUT.
std::optional<MenuArtImage> decode_menu_texture(std::span<const uint8_t> tmx, uint32_t tbp, MenuArtLook look);

// Whether a source is one of these ("game-menu/...")
bool is_game_menu_source(std::string_view source);

// The image a source names, read from the disc; nullopt (and a line in the log) when the source is not one or the
// disc has no such texture
std::optional<MenuArtImage> load_game_menu_art(std::string_view source);

// Whether the disc has menu/menu.tmx (the menu then dresses its window in the game's art)
bool game_menu_art_on_disc();

} // namespace ui
