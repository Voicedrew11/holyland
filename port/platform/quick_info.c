/* quick_info.c: a quick save's PC block (port/include/port/quick_info.h), packed and unpacked; the file
   around it is port/platform/quicksave.c's. Every number little-endian whatever the build, byte by byte. */
#include <string.h>

#include "port/quick_info.h"

#define INFO_SIZE_NEIGHBOUR 68u /* the files with the neighbour, before the live world */
#define MAP_SIZE 140u           /* one map's live world */

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v) {
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static void putf(uint8_t *p, float f) {
    uint32_t v;

    memcpy(&v, &f, 4);
    put32(p, v);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t get64(const uint8_t *p) {
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

static float getf(const uint8_t *p) {
    uint32_t v = get32(p);
    float f;

    memcpy(&f, &v, 4);
    return f;
}

static void pack_maps(uint8_t *p, const PortQuickInfo *info) {
    int i;

    p[0] = (uint8_t)(info->map_count < 0 ? 0 : info->map_count > 2 ? 2 : info->map_count);
    for (i = 0; i < p[0]; i++) {
        const PortQuickMap *map = &info->maps[i];
        uint8_t *m = p + 4 + MAP_SIZE * (uint32_t)i;

        m[0] = (uint8_t)map->tag;
        m[1] = (uint8_t)map->spawn_count;
        m[2] = (uint8_t)map->item_count;
        put64(m + 4, map->items_lying);
        memcpy(m + 12, map->spawns, PORT_QUICK_SPAWNS);
    }
}

static void unpack_maps(const uint8_t *p, PortQuickInfo *info) {
    int i;

    info->map_count = p[0] > 2 ? 0 : p[0]; /* (more than two: not a file this program wrote) */
    for (i = 0; i < info->map_count; i++) {
        PortQuickMap *map = &info->maps[i];
        const uint8_t *m = p + 4 + MAP_SIZE * (uint32_t)i;

        map->tag = m[0];
        map->spawn_count = m[1];
        map->item_count = m[2];
        map->items_lying = get64(m + 4);
        memcpy(map->spawns, m + 12, PORT_QUICK_SPAWNS);
    }
}

void port_quick_info_pack(uint8_t *p, const PortQuickInfo *info) {
    int i;

    memset(p, 0, PORT_QUICK_INFO_SIZE);
    put64(p + 0, (uint64_t)info->time);
    put32(p + 8, (uint32_t)info->map_tag);
    for (i = 0; i < 3; i++) {
        putf(p + 12 + 4 * i, info->position[i]);
        putf(p + 24 + 4 * i, info->rotation[i]);
    }
    put32(p + 36, (uint32_t)info->hp);
    put32(p + 40, (uint32_t)info->hp_max);
    put32(p + 44, (uint32_t)info->mp);
    put32(p + 48, (uint32_t)info->mp_max);
    put32(p + 52, (uint32_t)info->level);
    put32(p + 56, (uint32_t)info->gold);
    put32(p + 60, info->play_seconds);
    p[64] = (uint8_t)(info->neighbour_origin < 0 ? 0xFF : info->neighbour_map);
    p[65] = (uint8_t)(info->neighbour_origin < 0 ? 0xFF : info->neighbour_map >> 8);
    p[66] = (uint8_t)(info->neighbour_origin < 0 ? 0xFF : info->neighbour_origin);
    p[67] = (uint8_t)(info->neighbour_origin >= 0 && info->neighbour_set_up);
    pack_maps(p + INFO_SIZE_NEIGHBOUR, info);
}

void port_quick_info_unpack(const uint8_t *p, uint32_t size, PortQuickInfo *info) {
    int i;

    memset(info, 0, sizeof(*info));
    info->time = (int64_t)get64(p + 0);
    info->map_tag = (int32_t)get32(p + 8);
    for (i = 0; i < 3; i++) {
        info->position[i] = getf(p + 12 + 4 * i);
        info->rotation[i] = getf(p + 24 + 4 * i);
    }
    info->hp = (int32_t)get32(p + 36);
    info->hp_max = (int32_t)get32(p + 40);
    info->mp = (int32_t)get32(p + 44);
    info->mp_max = (int32_t)get32(p + 48);
    info->level = (int32_t)get32(p + 52);
    info->gold = (int32_t)get32(p + 56);
    info->play_seconds = get32(p + 60);
    info->neighbour_origin = info->neighbour_map = -1;
    if (size >= INFO_SIZE_NEIGHBOUR) {
        info->neighbour_known = !(p[67] & 2);
        info->made_up = (p[67] & 2) != 0;
        if (p[66] != 0xFF && !info->made_up) {
            info->neighbour_origin = p[66];
            info->neighbour_map = p[64] | p[65] << 8;
            info->neighbour_set_up = p[67] & 1;
        }
    }
    if (size >= PORT_QUICK_INFO_SIZE && !info->made_up) {
        unpack_maps(p + INFO_SIZE_NEIGHBOUR, info);
    }
}
