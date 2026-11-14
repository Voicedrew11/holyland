// The PC renderer's spare surface images.
//
// Each game frame kf4-pc copies every surface for the pump's canvas to draw the pictures between on
// (Canvas::copy_surfaces). Made new each time, that was about 600 MB of screen-sized images made and freed a game
// frame at 4x with MSAA 2 and 16 bits a channel, freed only as Granite's frame contexts came round: the game's own
// GPU memory stood at 4 GB and swung by more than 1 GB within a quarter second (HANDOFF_2026-11-13.md, 3).
//
// The images the pump's canvas lets go when a new frame's copies arrive (Canvas::start_from) are kept here instead,
// and the next copies are made into those of the same kind: two sets of images in all, one the pump draws on, one
// spare or being filled.
//
// A spare is taken only by a canvas whose commands are submitted after those of the canvas that let it go (the
// caller's to keep: start_from submits before it keeps). Spares not taken for KEPT_ROUNDS calls of keep are let go:
// surfaces of a scale, a colour precision or a sample count no longer used.
//
// Header only. One lock: keep and take may be called from different threads.
#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace pcr {

// What makes an image fit a copy: an image of one kind holds any surface of that kind
struct ImageKind {
    uint32_t width = 0, height = 0; // pixels
    uint32_t format = 0;            // VkFormat
    uint32_t samples = 1;
    uint32_t usage = 0;             // VkImageUsageFlags
    bool operator==(const ImageKind &) const = default;
};

// Handle: what holds an image (Vulkan::ImageHandle; the test's own). Empty when default-made, and tested as a bool.
template <typename Handle>
class SpareImages {
public:
    static constexpr uint64_t KEPT_ROUNDS = 2; // calls of keep a spare outlives untaken

    // Images let go, each with its kind; the spares kept before KEPT_ROUNDS calls ago let go for good
    void keep(std::vector<std::pair<ImageKind, Handle>> &&images) {
        std::vector<Spare> gone; // (their handles let go outside the lock)
        {
            std::lock_guard<std::mutex> hold(lock_);
            round_++;
            std::vector<Spare> kept;
            kept.reserve(spares_.size() + images.size());
            for (Spare &s : spares_) {
                (round_ - s.round >= KEPT_ROUNDS ? gone : kept).push_back(std::move(s));
            }
            for (auto &[kind, image] : images) {
                if (image) {
                    kept.push_back(Spare{kind, std::move(image), round_});
                }
            }
            spares_ = std::move(kept);
        }
        images.clear();
    }

    // A spare of this kind (the one kept longest), or an empty handle
    Handle take(const ImageKind &kind) {
        std::lock_guard<std::mutex> hold(lock_);
        for (size_t i = 0; i < spares_.size(); i++) {
            if (spares_[i].kind == kind) {
                Handle image = std::move(spares_[i].image);
                spares_.erase(spares_.begin() + std::ptrdiff_t(i));
                return image;
            }
        }
        return Handle();
    }

    size_t count() const {
        std::lock_guard<std::mutex> hold(lock_);
        return spares_.size();
    }

private:
    struct Spare {
        ImageKind kind;
        Handle image;
        uint64_t round = 0; // (the call of keep it came in)
    };
    mutable std::mutex lock_;
    std::vector<Spare> spares_;
    uint64_t round_ = 0;
};

} // namespace pcr
