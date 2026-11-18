// RmlUi's drawing, recorded (port/ui/ui_render.hpp)
#include "ui_render.hpp"

#include <RmlUi/Core/Log.h>

#include <cstdio>
#include <cstring>

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

} // namespace

void Recorder::begin_frame(int width, int height) {
    frame_.width = width;
    frame_.height = height;
    frame_.vertices.clear();
    frame_.indices.clear();
    frame_.draws.clear();
    frame_.uploads.clear();
    frame_.releases.clear();
    clipping_ = false;
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

Rml::TextureHandle Recorder::LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) {
    (void)source;
    dimensions = {0, 0};
    not_drawn("an image from a file");
    return 0;
}

Rml::TextureHandle Recorder::GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) {
    if (dimensions.x <= 0 || dimensions.y <= 0 || source.size() != size_t(dimensions.x) * size_t(dimensions.y) * 4) {
        return 0;
    }
    TextureUpload upload;
    upload.id = next_texture_++;
    upload.width = dimensions.x;
    upload.height = dimensions.y;
    upload.rgba.assign(source.begin(), source.end());
    frame_.uploads.push_back(std::move(upload));
    return Rml::TextureHandle(frame_.uploads.back().id);
}

void Recorder::ReleaseTexture(Rml::TextureHandle texture) {
    if (texture != 0) {
        frame_.releases.push_back(TextureId(texture));
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
