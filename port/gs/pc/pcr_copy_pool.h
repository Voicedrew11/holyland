// The PC renderer's surface texture copies, used again.
//
// A draw whose texture is a surface the game drew (the frame read back for a fade, a blur, the water) reads a copy of
// it (Canvas: surface_texture), an image the texture's size at the canvas's scale. Made new for each such draw, at 4x
// with native widescreen and 16 bits a channel that was a 96 MB image made, cleared whole, copied into, read once and
// let go, about 300 times a second: 0.2 ms of the GPU a picture, and the game's own GPU memory swinging by 0.8 GB as
// Granite's frame contexts let the images go (PC_OPTIMIZATION.md, 2026-11-14).
//
// The copies are kept here and given again:
// - An image is given to one draw at a time: it comes with a lease, and is given again only when every copy of that
//   lease is gone (the batch drawn and let go).
// - What was read outside the copied rectangle must be zeros. An image is cleared when it is new, and when the
//   rectangle to copy does not cover all that was written into it since its last clear; a copy of the same rectangle
//   as the last needs no clear.
// - An image not given for KEPT_TAKES takes is let go: copies of a scale or a size no longer used.
//
// One thread: a canvas's own (the image given again is written by commands recorded after those of the draw that
// read it, in the one command stream).
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "pcr_spares.h" // ImageKind

namespace pcr {

// Handle: what holds an image (Vulkan::ImageHandle; the test's own). Empty when default-made, and tested as a bool.
template <typename Handle>
class CopyPool {
public:
    static constexpr uint64_t KEPT_TAKES = 64; // takes an image outlives ungiven

    using Lease = std::shared_ptr<const int>;

    struct Copy {
        Handle image;             // empty: none could be made
        Lease lease;              // held by whatever reads the image; the image is given again when all are gone
        bool clear_first = false; // the image is to be cleared before the rectangle is copied into it
    };

    // An image of this kind to copy a rectangle of width x height pixels into, at its origin. make(): a new image of
    // the kind (an empty handle when there is no memory for one).
    template <typename Make>
    Copy take(const ImageKind &kind, uint32_t width, uint32_t height, Make &&make) {
        takes_++;
        let_go_of_the_unused();
        for (Kept &k : kept_) {
            if (k.kind == kind && k.lease.use_count() == 1) {
                bool clear = width < k.written_width || height < k.written_height;
                k.written_width = width;
                k.written_height = height;
                k.taken_at = takes_;
                return Copy{k.image, k.lease, clear};
            }
        }
        Handle image = make();
        if (!image) {
            return Copy{};
        }
        kept_.push_back(Kept{kind, image, std::make_shared<const int>(0), width, height, takes_});
        return Copy{std::move(image), kept_.back().lease, true};
    }

    size_t count() const {
        return kept_.size();
    }

    void clear() {
        kept_.clear();
    }

private:
    struct Kept {
        ImageKind kind;
        Handle image;
        Lease lease;
        uint32_t written_width = 0, written_height = 0; // (since its last clear: the last copy's rectangle)
        uint64_t taken_at = 0;
    };

    void let_go_of_the_unused() {
        for (size_t i = 0; i < kept_.size();) {
            if (kept_[i].lease.use_count() == 1 && takes_ - kept_[i].taken_at > KEPT_TAKES) {
                kept_.erase(kept_.begin() + std::ptrdiff_t(i));
            } else {
                i++;
            }
        }
    }

    std::vector<Kept> kept_;
    uint64_t takes_ = 0;
};

} // namespace pcr
