// Dead writes in a walk drawn round and round (port/gs/pc/pcr_dead_writes.h)
#include "pcr_dead_writes.h"

#include <cstdio>

using namespace pcr;

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace {

WalkStep step(uint8_t reads, uint8_t writes, uint8_t kills, int32_t x1 = 639, int32_t y1 = 447, uint32_t colour = 0,
              uint32_t depth = 280) {
    WalkStep s;
    s.colour = colour;
    s.depth = depth;
    s.x0 = 0;
    s.y0 = 0;
    s.x1 = x1;
    s.y1 = y1;
    s.reads = reads;
    s.writes = writes;
    s.kills = kills;
    return s;
}

// King's Field IV's frame at 009: the clear twice, the scene, the alpha set to 0, Z cleared, the fade, the brightness
void test_the_games_frame() {
    const WalkStep clear = step(0, DEAD_ALL, DEAD_ALL);
    const WalkStep scene = step(DEAD_RGB | DEAD_Z, DEAD_ALL, 0);
    const WalkStep alpha_to_0 = step(0, DEAD_ALPHA, DEAD_ALPHA);
    const WalkStep z_to_0 = step(0, DEAD_Z, DEAD_Z);
    const WalkStep fade = step(DEAD_RGB, DEAD_RGB | DEAD_ALPHA, DEAD_RGB | DEAD_ALPHA);
    WalkStep brightness = step(0, DEAD_RGB | DEAD_ALPHA, DEAD_RGB | DEAD_ALPHA);
    brightness.reads_anywhere = DEAD_RGB | DEAD_ALPHA; // (the screen read as a texture)
    const WalkStep walk[] = {clear, clear, scene, scene, alpha_to_0, z_to_0, fade, brightness};
    std::vector<bool> dead = dead_writes(walk);
    CHECK(dead[0]);  // the first clear: the second writes all of it again
    CHECK(!dead[1]); // the second: the scene reads it
    CHECK(!dead[2] && !dead[3]);
    CHECK(dead[4]);  // the alpha: the fade writes it before anything reads it
    CHECK(dead[5]);  // Z: nothing tests it before the next picture's clear
    CHECK(!dead[6]); // the fade: the brightness pass reads the screen
    CHECK(!dead[7]); // the brightness: the picture
}

void test_readers_keep_a_write() {
    // the alpha set, then a DATE draw reads it before the fade
    const WalkStep alpha = step(0, DEAD_ALPHA, DEAD_ALPHA);
    const WalkStep date = step(DEAD_ALPHA | DEAD_RGB, DEAD_RGB, 0);
    const WalkStep fade = step(DEAD_RGB, DEAD_RGB | DEAD_ALPHA, DEAD_RGB | DEAD_ALPHA);
    const WalkStep clear = step(0, DEAD_ALL, DEAD_ALL);
    {
        const WalkStep walk[] = {clear, alpha, date, fade};
        CHECK(!dead_writes(walk)[1]);
    }
    // Z cleared at the end, and the walk's first draw tests Z before any clear: read, round the end
    {
        const WalkStep z = step(0, DEAD_Z, DEAD_Z);
        const WalkStep tests_z = step(DEAD_Z | DEAD_RGB, DEAD_ALL, 0);
        const WalkStep walk[] = {tests_z, clear, tests_z, z};
        CHECK(!dead_writes(walk)[3]);
    }
    // a killer that covers less than the write leaves it live
    {
        const WalkStep small_fade = step(DEAD_RGB, DEAD_RGB | DEAD_ALPHA, DEAD_RGB | DEAD_ALPHA, 319, 447);
        const WalkStep reads_alpha = step(DEAD_ALPHA, DEAD_RGB, 0);
        const WalkStep walk[] = {clear, alpha, small_fade, reads_alpha};
        CHECK(!dead_writes(walk)[1]);
    }
    // a killer into another buffer does not kill; a texture read from a surface reads every buffer's colour and alpha
    {
        const WalkStep other = step(0, DEAD_RGB | DEAD_ALPHA, DEAD_RGB | DEAD_ALPHA, 639, 447, 466);
        WalkStep from_surface = step(0, DEAD_RGB, 0, 127, 127, 466);
        from_surface.reads_anywhere = DEAD_RGB | DEAD_ALPHA;
        const WalkStep walk[] = {clear, alpha, other, from_surface, fade};
        CHECK(!dead_writes(walk)[1]);
    }
    // an upload or a copy (everything read anywhere) keeps Z too
    {
        const WalkStep z = step(0, DEAD_Z, DEAD_Z);
        WalkStep upload;
        upload.reads_anywhere = DEAD_ALL;
        const WalkStep walk[] = {clear, z, upload, clear};
        CHECK(!dead_writes(walk)[1]);
    }
}

void test_colour_is_always_looked_at() {
    // the last step's colour is the picture's, and a colour nothing in the walk reads is still shown
    const WalkStep clear = step(0, DEAD_ALL, DEAD_ALL);
    const WalkStep draw = step(0, DEAD_RGB, 0, 99, 99);
    const WalkStep walk[] = {clear, draw};
    std::vector<bool> dead = dead_writes(walk);
    CHECK(!dead[0]); // (the draw covers less: the clear's colour shows round it)
    CHECK(!dead[1]);
    const WalkStep one[] = {clear};
    CHECK(!dead_writes(one)[0]);
    // a step that writes nothing is not called dead
    const WalkStep nothing = step(DEAD_RGB, 0, 0);
    const WalkStep two[] = {clear, nothing};
    CHECK(!dead_writes(two)[1]);
    CHECK(dead_writes(std::span<const WalkStep>()).empty());
}

} // namespace

int main() {
    test_the_games_frame();
    test_readers_keep_a_write();
    test_colour_is_always_looked_at();
    if (failures) {
        std::printf("pcr_dead_writes: %d failed\n", failures);
        return 1;
    }
    std::printf("pcr_dead_writes: all passed\n");
    return 0;
}
