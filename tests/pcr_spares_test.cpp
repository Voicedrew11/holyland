// The PC renderer's spare surface images (port/gs/pc/pcr_spares.h): a copy is made into a spare of its kind only, a
// spare is given once, those not taken are let go after KEPT_ROUNDS, and a steady game needs two sets of images.
#include "pcr_spares.h"

#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

using namespace pcr;

namespace {

int failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

using Handle = std::shared_ptr<int>; // (an image: let go when the last handle is)
using Spares = SpareImages<Handle>;
using LetGo = std::vector<std::pair<ImageKind, Handle>>;

const ImageKind colour = {3840, 1792, 97, 1, 0x97};
const ImageKind colour_ms = {3840, 1792, 97, 2, 0x13};
const ImageKind depth = {3840, 1792, 130, 2, 0x23};

void taken_by_kind_once() {
    Spares spares;
    CHECK(!spares.take(colour));
    Handle a = std::make_shared<int>(1), b = std::make_shared<int>(2);
    LetGo let_go = {{colour, a}, {depth, b}, {colour, Handle()}}; // (an empty handle is not a spare)
    spares.keep(std::move(let_go));
    CHECK(let_go.empty());
    CHECK(spares.count() == 2);
    CHECK(!spares.take(colour_ms)); // (the same size, other samples)
    ImageKind wider = colour;
    wider.width = 5120;
    CHECK(!spares.take(wider));
    ImageKind other_format = colour;
    other_format.format = 37;
    CHECK(!spares.take(other_format));
    ImageKind other_usage = colour;
    other_usage.usage |= 8;
    CHECK(!spares.take(other_usage));
    CHECK(spares.take(colour) == a);
    CHECK(!spares.take(colour)); // (given once)
    CHECK(spares.take(depth) == b);
    CHECK(spares.count() == 0);
}

void longest_kept_first() {
    Spares spares;
    Handle a = std::make_shared<int>(1), b = std::make_shared<int>(2);
    spares.keep(LetGo{{colour, a}});
    spares.keep(LetGo{{colour, b}});
    CHECK(spares.take(colour) == a);
    CHECK(spares.take(colour) == b);
}

void untaken_let_go() {
    Spares spares;
    Handle a = std::make_shared<int>(1);
    std::weak_ptr<int> watch = a;
    spares.keep(LetGo{{colour, std::move(a)}});
    for (uint64_t i = 1; i < Spares::KEPT_ROUNDS; i++) {
        spares.keep(LetGo{});
        CHECK(!watch.expired());
    }
    spares.keep(LetGo{});
    CHECK(watch.expired()); // (the image itself let go, not only forgotten)
    CHECK(spares.count() == 0);
}

// A game frame: the copies made (spares taken, else new), the canvas that draws on them letting the frame before's go
void two_sets_when_steady() {
    Spares spares;
    const std::vector<ImageKind> surfaces = {colour, colour_ms, depth, colour, colour_ms};
    int made = 0;
    LetGo drawn_on;
    for (int frame = 0; frame < 100; frame++) {
        LetGo copies;
        for (const ImageKind &kind : surfaces) {
            Handle image = spares.take(kind);
            if (!image) {
                image = std::make_shared<int>(made++);
            }
            copies.emplace_back(kind, std::move(image));
        }
        spares.keep(std::move(drawn_on));
        drawn_on = std::move(copies);
    }
    CHECK(made == 2 * int(surfaces.size()));
    // (the scale changed: the old kind's spares let go, two sets of the new kind made)
    ImageKind bigger = colour;
    bigger.width *= 2;
    std::weak_ptr<int> old = drawn_on[0].second;
    made = 0;
    for (int frame = 0; frame < 10; frame++) {
        LetGo copies;
        Handle image = spares.take(bigger);
        if (!image) {
            image = std::make_shared<int>(made++);
        }
        copies.emplace_back(bigger, std::move(image));
        spares.keep(std::move(drawn_on));
        drawn_on = std::move(copies);
    }
    CHECK(made == 2);
    CHECK(old.expired());
    CHECK(spares.count() == 1);
}

// (two threads, as kf4-pc and the pump: for the thread sanitizer)
void kept_and_taken_together() {
    Spares spares;
    std::thread keeper([&] {
        for (int i = 0; i < 2000; i++) {
            spares.keep(LetGo{{colour, std::make_shared<int>(i)}});
        }
    });
    int taken = 0;
    for (int i = 0; i < 2000; i++) {
        taken += spares.take(colour) ? 1 : 0;
    }
    keeper.join();
    CHECK(taken <= 2000);
    CHECK(spares.count() <= Spares::KEPT_ROUNDS);
}

} // namespace

int main() {
    taken_by_kind_once();
    longest_kept_first();
    untaken_let_go();
    two_sets_when_steady();
    kept_and_taken_together();
    if (failures != 0) {
        std::printf("pcr_spares_test: %d failures\n", failures);
        return 1;
    }
    std::printf("pcr_spares_test: ok\n");
    return 0;
}
