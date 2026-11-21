// RmlUi's drawing, recorded (port/ui/ui_render.hpp)
#include "ui_render.hpp"

#include <RmlUi/Core/Log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "ui_assets.hpp"

namespace ui {

namespace {

// Logs a drawing feature the recorder does not draw, once each
void not_drawn(const char *what) {
    static std::vector<const char *> told;
    for (const char *t : told) {
        if (t == what) {
            return;
        }
    }
    told.push_back(what);
    std::fprintf(stderr, "[ui] RmlUi asked for %s, which the menu does not draw\n", what);
}

// An alpha mask's header (tools/ui_masks.py): "KF4A", the width and the height, little-endian
const size_t MASK_HEADER = 12;
const uint32_t MASK_SIDE_MAX = 8192;

uint32_t little_endian(const unsigned char *bytes) {
    return uint32_t(bytes[0]) | uint32_t(bytes[1]) << 8 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 24;
}

} // namespace

void Recorder::begin_frame(int width, int height) {
    frame_.width = width;
    frame_.height = height;
    frame_.vertices.clear();
    frame_.indices.clear();
    frame_.draws.clear();
    frame_.uploads = std::move(held_uploads_);
    frame_.releases = std::move(held_releases_);
    held_uploads_.clear();
    held_releases_.clear();
    clipping_ = false;
    recording_ = true;
}

void Recorder::end_frame() {
    recording_ = false;
}

TextureId Recorder::add_upload(TextureUpload upload) {
    upload.id = next_texture_++;
    std::vector<TextureUpload> &to = recording_ ? frame_.uploads : held_uploads_;
    to.push_back(std::move(upload));
    return to.back().id;
}

Rml::CompiledGeometryHandle Recorder::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                      Rml::Span<const int> indices) {
    static_assert(sizeof(Rml::Vertex) == sizeof(Vertex), "Rml::Vertex is copied as ui::Vertex");
    Geometry geometry;
    geometry.vertices.resize(vertices.size());
    std::memcpy(geometry.vertices.data(), vertices.data(), vertices.size() * sizeof(Vertex));
    geometry.indices.reserve(indices.size());
    for (int index : indices) {
        // (RmlUi's indices are into its own vertices: one out of them would read past the frame's)
        if (index < 0 || size_t(index) >= vertices.size()) {
            Rml::Log::Message(Rml::Log::LT_ERROR, "[ui] a geometry's index is out of its vertices");
            return 0;
        }
        geometry.indices.push_back(uint32_t(index));
    }
    uintptr_t handle = next_geometry_++;
    geometries_.emplace(handle, std::move(geometry));
    return Rml::CompiledGeometryHandle(handle);
}

void Recorder::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation,
                              Rml::TextureHandle texture) {
    auto found = geometries_.find(uintptr_t(handle));
    if (found == geometries_.end() || found->second.indices.empty()) {
        return;
    }
    const Geometry &geometry = found->second;
    Draw draw = {};
    draw.first_index = uint32_t(frame_.indices.size());
    draw.index_count = uint32_t(geometry.indices.size());
    draw.vertex_offset = uint32_t(frame_.vertices.size());
    draw.texture = TextureId(texture);
    draw.translate_x = translation.x;
    draw.translate_y = translation.y;
    draw.clipped = clipping_;
    draw.clip = clip_;
    frame_.vertices.insert(frame_.vertices.end(), geometry.vertices.begin(), geometry.vertices.end());
    frame_.indices.insert(frame_.indices.end(), geometry.indices.begin(), geometry.indices.end());
    frame_.draws.push_back(draw);
}

void Recorder::ReleaseGeometry(Rml::CompiledGeometryHandle handle) {
    geometries_.erase(uintptr_t(handle));
}

// Only the menu's own alpha masks (.alpha, tools/ui_masks.py): white, premultiplied, coloured by the sheet's image-color
Rml::TextureHandle Recorder::LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) {
    dimensions = {0, 0};
    const UiAsset *file = ui_asset(source);
    if (file == nullptr || file->size < MASK_HEADER || std::memcmp(file->data, "KF4A", 4) != 0) {
        not_drawn("an image other than the menu's alpha masks");
        return 0;
    }
    uint32_t width = little_endian(file->data + 4), height = little_endian(file->data + 8);
    if (width == 0 || height == 0 || width > MASK_SIDE_MAX || height > MASK_SIDE_MAX ||
        file->size - MASK_HEADER != size_t(width) * size_t(height)) {
        Rml::Log::Message(Rml::Log::LT_ERROR, "[ui] %s is not a whole alpha mask", source.c_str());
        return 0;
    }
    TextureUpload upload;
    upload.width = int(width);
    upload.height = int(height);
    upload.rgba.resize(size_t(width) * size_t(height) * 4);
    const unsigned char *alpha = file->data + MASK_HEADER;
    for (size_t i = 0; i < size_t(width) * size_t(height); i++) {
        std::memset(&upload.rgba[i * 4], alpha[i], 4);
    }
    dimensions = {int(width), int(height)};
    return Rml::TextureHandle(add_upload(std::move(upload)));
}

Rml::TextureHandle Recorder::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    if (dimensions.x <= 0 || dimensions.y <= 0 || source.size() != size_t(dimensions.x) * size_t(dimensions.y) * 4) {
        return 0;
    }
    TextureUpload upload;
    upload.width = dimensions.x;
    upload.height = dimensions.y;
    upload.rgba.assign(source.begin(), source.end());
    return Rml::TextureHandle(add_upload(std::move(upload)));
}

void Recorder::ReleaseTexture(Rml::TextureHandle texture) {
    if (texture != 0) {
        (recording_ ? frame_.releases : held_releases_).push_back(TextureId(texture));
    }
}

void Recorder::EnableScissorRegion(bool enable) {
    clipping_ = enable;
}

void Recorder::SetScissorRegion(Rml::Rectanglei region) {
    clip_ = {region.Left(), region.Top(), region.Width(), region.Height()};
}

void Recorder::SetTransform(const Rml::Matrix4f *transform) {
    if (transform != nullptr) {
        not_drawn("a transform");
    }
}

void Recorder::EnableClipMask(bool enable) {
    if (enable) {
        not_drawn("a clip mask");
    }
}

} // namespace ui
