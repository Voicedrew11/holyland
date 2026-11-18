// One picture of the player menu, as triangles (port/include/port/player_ui.h): what port/ui/ui_render.cpp records
// from RmlUi and the Vulkan presenter draws (port/platform/present_vk_ui.cpp). No Vulkan here.
#pragma once

#include <cstdint>
#include <vector>

namespace ui {

// RmlUi's own vertex (Rml::Vertex): a position in the window's pixels, a premultiplied colour, a texture place
struct Vertex {
    float x, y;
    uint8_t r, g, b, a;
    float u, v;
};
static_assert(sizeof(Vertex) == 20, "ui::Vertex must match Rml::Vertex");

// 0: no texture (the vertex colours alone)
using TextureId = uint32_t;

// A rectangle in the window's pixels
struct Rect {
    int32_t x, y, width, height;
};

// One draw: index_count indices from first_index, each plus vertex_offset, moved by the translation
struct Draw {
    uint32_t first_index;
    uint32_t index_count;
    uint32_t vertex_offset;
    TextureId texture;
    float translate_x, translate_y;
    bool clipped; // only inside clip
    Rect clip;
};

// A texture made since the last frame (the fonts' glyphs, images): RGBA, premultiplied, rows top first
struct TextureUpload {
    TextureId id;
    int32_t width, height;
    std::vector<uint8_t> rgba;
};

struct Frame {
    int32_t width = 0, height = 0; // the window's pixels
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<Draw> draws;
    // textures to make before this frame's draws, and those no longer used (after its draws)
    std::vector<TextureUpload> uploads;
    std::vector<TextureId> releases;
};

} // namespace ui

// (the presenter, after port_player_ui_draw) the frame to draw, or nullptr when the menu draws nothing. Its uploads
// and releases are handed over once: the next port_player_ui_draw starts them afresh.
const ui::Frame *player_ui_frame();
