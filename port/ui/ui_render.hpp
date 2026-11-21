// RmlUi's drawing, recorded (port/ui/ui_frame.hpp): its geometry and textures kept here, each picture's draws
// written into a ui::Frame for the presenter. Only what the menu's style sheets use: triangles, textures and
// scissor clipping. Transforms, clip masks, layers, filters and shaders are not drawn (RmlUi then draws the element
// without them); the first use of each is logged once.
#pragma once

#include <RmlUi/Core/RenderInterface.h>

#include <unordered_map>
#include <vector>

#include "ui_frame.hpp"

namespace ui {

class Recorder final : public Rml::RenderInterface {
public:
    // Starts a picture of width x height pixels: the last frame's draws, uploads and releases (handed to the
    // presenter) are emptied. Textures RmlUi makes or lets go of from here on are this frame's, and those it made or
    // let go of between frames (loading the document: the logo) are given to it too.
    void begin_frame(int width, int height);
    // The frame is done and goes to the presenter: textures made or let go of from now wait for the next one
    void end_frame();
    const Frame &frame() const {
        return frame_;
    }

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry, Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;
    // Only the menu's alpha masks (port/ui/res/*.alpha, tools/ui_masks.py); other images are not drawn (0, nothing)
    Rml::TextureHandle LoadTexture(Rml::Vector2i &dimensions, const Rml::String &source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source, Rml::Vector2i dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;
    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;
    void SetTransform(const Rml::Matrix4f *transform) override;
    void EnableClipMask(bool enable) override;

private:
    // a new texture's upload into this frame, or the next when between frames; its id
    TextureId add_upload(TextureUpload upload);

    struct Geometry {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
    };
    std::unordered_map<uintptr_t, Geometry> geometries_;
    uintptr_t next_geometry_ = 1;
    TextureId next_texture_ = 1;
    Frame frame_;
    bool recording_ = false;                // between begin_frame and end_frame
    std::vector<TextureUpload> held_uploads_; // made between frames, for the next
    std::vector<TextureId> held_releases_;
    bool clipping_ = false;
    Rect clip_ = {};
};

} // namespace ui
