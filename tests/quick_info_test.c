/* The quick save's PC block (port/platform/quick_info.c): what is packed comes back, and the older,
   shorter blocks unpack as files without the newer parts */
#include <stdio.h>
#include <string.h>

#include "port/quick_info.h"

static int failures;

#define CHECK(cond)                                                                                                  \
    do {                                                                                                             \
        if (!(cond)) {                                                                                               \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #cond);                                               \
            failures++;                                                                                              \
        }                                                                                                            \
    } while (0)

static PortQuickInfo sample(void) {
    PortQuickInfo info;
    int i;

    memset(&info, 0, sizeof(info));
    info.time = 1795000000;
    info.map_tag = 0x63;
    info.position[0] = -5854.61f;
    info.position[2] = -9786.06f;
    info.rotation[1] = -0.21f;
    info.hp = 50;
    info.hp_max = 50;
    info.level = 1;
    info.play_seconds = 4321;
    info.neighbour_known = 1;
    info.neighbour_origin = 6;
    info.neighbour_map = 0;
    info.neighbour_set_up = 1;
    info.map_count = 2;
    info.maps[0].tag = 0x63;
    info.maps[0].spawn_count = 13;
    info.maps[0].item_count = 3;
    info.maps[0].items_lying = 0x5;
    info.maps[0].spawns[2] = PORT_QUICK_SPAWN_DEAD | PORT_QUICK_SPAWN_NO_RESPAWN;
    info.maps[0].spawns[12] = PORT_QUICK_SPAWN_DEAD | PORT_QUICK_SPAWN_ROLLED;
    info.maps[1].tag = 0x00;
    info.maps[1].spawn_count = PORT_QUICK_SPAWNS;
    info.maps[1].item_count = PORT_QUICK_FIXED_ITEMS;
    info.maps[1].items_lying = 0x8000000000000001ull;
    for (i = 0; i < PORT_QUICK_SPAWNS; i++) {
        info.maps[1].spawns[i] = (uint8_t)(i % 8);
    }
    return info;
}

static void round_trip(void) {
    PortQuickInfo info = sample(), back;
    uint8_t block[PORT_QUICK_INFO_SIZE];

    port_quick_info_pack(block, &info);
    port_quick_info_unpack(block, sizeof(block), &back);
    CHECK(memcmp(&info, &back, sizeof(info)) == 0);
}

/* a later program's longer block: what this one knows comes back, the rest is skipped */
static void longer_block(void) {
    PortQuickInfo info = sample(), back;
    uint8_t block[PORT_QUICK_INFO_SIZE + 32];

    memset(block, 0xAB, sizeof(block));
    port_quick_info_pack(block, &info);
    port_quick_info_unpack(block, sizeof(block), &back);
    CHECK(memcmp(&info, &back, sizeof(info)) == 0);
}

/* the files before the live world (68 bytes) and before the neighbour (64): nothing of what they lack */
static void older_blocks(void) {
    PortQuickInfo info = sample(), back;
    uint8_t block[PORT_QUICK_INFO_SIZE];

    port_quick_info_pack(block, &info);
    port_quick_info_unpack(block, 68, &back);
    CHECK(back.map_count == 0);
    CHECK(back.neighbour_known == 1 && back.neighbour_origin == 6 && back.neighbour_set_up == 1);
    CHECK(back.hp == 50 && back.position[2] == info.position[2]);

    port_quick_info_unpack(block, PORT_QUICK_INFO_SIZE_MIN, &back);
    CHECK(back.map_count == 0);
    CHECK(back.neighbour_known == 0 && back.neighbour_origin == -1 && back.neighbour_map == -1);
    CHECK(back.play_seconds == 4321);
}

/* a tool's made-up file (map_tour.py) has no live world, whatever its bytes say */
static void made_up(void) {
    PortQuickInfo info = sample(), back;
    uint8_t block[PORT_QUICK_INFO_SIZE];

    port_quick_info_pack(block, &info);
    block[67] |= 2;
    port_quick_info_unpack(block, sizeof(block), &back);
    CHECK(back.made_up == 1);
    CHECK(back.map_count == 0);
    CHECK(back.neighbour_origin == -1);
}

/* more maps than two is not a file this program wrote: none taken */
static void too_many_maps(void) {
    PortQuickInfo info = sample(), back;
    uint8_t block[PORT_QUICK_INFO_SIZE];

    port_quick_info_pack(block, &info);
    block[68] = 3;
    port_quick_info_unpack(block, sizeof(block), &back);
    CHECK(back.map_count == 0);
}

int main(void) {
    round_trip();
    longer_block();
    older_blocks();
    made_up();
    too_many_maps();
    if (failures != 0) {
        fprintf(stderr, "quick_info_test: %d failed\n", failures);
        return 1;
    }
    printf("quick_info_test: all passed\n");
    return 0;
}
