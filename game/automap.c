/* automap.c: PC-only. The automap mod's game side (port/include/port/automap.h, docs/port/AUTOMAP.md): once a
   game frame (Automap_Poll, from Quick_PollGame) it reads the map the player is in from the game's own
   structures and keeps what the overlay draws (port/overlay/automap_view.cpp). It only reads the game: with
   the mod off it returns at once, and with it on the game runs as it would without it.

   The floor plan. A map is made of records (MapSlot.records, MapRecordBounds), each with a model and a
   collision mesh: strips of triangles in six lists (CollisionMesh): walls, floors, more walls, entity
   barriers, hazards (lava, poison, the void: Collision_GetSurfaceEffect) and water. The game tests the
   player against exactly these, so they are the cleanest floor plan there is: they are put in the world
   with the record model's collision_matrix (Map_PlaceRecordModels) and drawn top down into cells of
   PORT_AUTOMAP_CELL units:
   - floors, water and hazards: each cell's centre is dropped onto the triangles over it; a cell keeps up to
     PORT_AUTOMAP_LAYERS heights (a bridge over a river, a balcony over a hall), heights less than
     MERGE apart being one layer (a lava floor is its hazard);
   - walls: a steep wall triangle marks the cells its edges cross, a sloped one (a cave's rock) the cells
     under it at its height there. A floor layer is a wall where such a wall stands on it up to a body's
     height (WALL_LOW to WALL_HIGH above it); a cell with no floor keeps the wall's top and bottom, so that
     solid rock beside a floor is drawn as wall.
   The view picks, in each cell, the layer the player would see at his height (the highest not far above his
   feet) and shades floors by their height against his.

   The fog of war: rays go out from the player along the floor (a step of at most CLIMB a cell), revealing
   each layer they cross, stopping at a wall; looking down off a ledge reveals the floor below. The explored
   layers are kept per map, as runs (Automap_Serialize), and saved per save by port/platform/automap.c.

   The markers come from the map objects (map_slot_objects, their kind from param/obj.prm), the entities
   (entity_pool) and the items lying in the map (lying_items); their names from the game's own text
   (Message_GetText, item_name_defs) where it has them, and from game/automap_guide.c where it does not. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "collision/collision.h"
#include "core/game.h"
#include "entity/entity.h"
#include "item/item.h"
#include "map/map.h"
#include "model/model.h"
#include "player/player.h"
#include "port/automap.h"
#include "script/script.h"
#include "port/platform.h"
#include "port/savestate.h"
#include "port/teleport.h"
#include "automap_script.h"

extern PlayerState player_state;
extern MapSlot map_slots[];
extern int current_map_slot;
extern MapObject map_slot_objects[][0x200];
extern s32 map_object_counts[2];
extern Entity entity_pool[][0x80];
extern int map_spawn_counts[2];
extern ItemNode *lying_items;
extern GameDefinition item_name_defs[];
extern u8 item_counts[];
extern MapItemDef *map_item_defs[2];
extern s32 map_fixed_item_counts[2];
extern u8 map_item_pickup_counts[0x80][0x40];
int MapItem_UniqueNotCarried(int item);
int MapItem_UniqueNotInTradeStock(int item);
int MapItem_UniqueNotDropped(int item);
void sceVu0UnitMatrix(void *m);
void sceVu0RotMatrixX(void *dst, void *src, float angle);
void sceVu0RotMatrixY(void *dst, void *src, float angle);
void sceVu0RotMatrixZ(void *dst, void *src, float angle);
void sceVu0TransMatrix(void *dst, void *src, void *translation);
extern GameStatus game_status;
extern EntitySpawnInfo *map_spawn_infos[2];
extern s16 *map_item_model_kinds[2];
extern char *entity_params;
extern char map_object_defs[];
extern DroppedItemRecord dropped_item_records[0x80][16];
extern u8 map_script_flags[0x2000];
extern u8 entity_group_state[0xC80];
int Game_IsMapVisited(int flag);
int Game_GetObjectActive(int map, int entry);
int Game_GetObjectState(int map, int entry);
int Game_GetObjectFlag(int map, int entry, u32 bit);
int Game_GetTriggerItem(int map_tag, int trigger_id);

sptr Message_GetText(int message_id);
const char *Quick_WhyNotInPlay(void);
int Game_GetSpawnDeathCount(int map, int entry);
u32 Collision_GetSurfaceEffect(u32 first, u32 second, int third);
const char *Automap_CreatureName(int record);
const char *Automap_ObjectHint(int map, int object);
const char *Automap_SpawnHint(int map, int spawn, int record);
int Automap_PersonPage(int record);

#define CELL PORT_AUTOMAP_CELL
#define LAYERS PORT_AUTOMAP_LAYERS
#define NO_Y PORT_AUTOMAP_NO_Y
#define MAX_CELLS 1400       /* a side of the grid at most: 70,000 units */
#define MARGIN 600.0f        /* room around the floors */
#define MERGE 150            /* heights closer than this in a cell are one layer */
#define WALL_LOW 40          /* a wall stands on a floor when it rises above it by this... */
#define WALL_HIGH 150        /* ...and reaches down to within this of it (a ceiling or a lintel does not) */
#define STEEP 0.35f          /* a wall triangle whose normal's y is under this is upright */
#define CLIMB 150            /* the most the floor rises or falls from cell to cell along a ray */
#define REACH 3400.0f        /* how far the rays reveal */
#define RAYS 720
#define AIR_CELLS 24         /* a ray goes this far over a drop or over nothing before it stops */
#define FAR_CELLS 14         /* a DEATH layer this far from any floor is not drawn */
#define MAX_MAPS 128
#define MAX_MARKERS 1024

/* ---- State -------------------------------------------------------------------------------------- */
static PortAutomapPlan plan = {-1};
static PortAutomapLayer *layers;
static int16_t *solid_top, *solid_bottom;
static uint8_t *seen;
static uint32_t layout_hash;
static int built_slot = -1;           /* the slot and records the plan was built from */
static const char *built_records;

typedef struct {
    int map;
    uint8_t *data; /* the map's chunk, as Automap_Serialize writes it */
    size_t size;
} StoredMap;
static StoredMap stored[MAX_MAPS];
static int stored_count;

/* the markers of the map in hand: the player's (home_markers), or, while the browser builds one, the browsed
   map's (view_swap) */
static PortAutomapMarker home_markers[MAX_MARKERS], browsed_markers[MAX_MARKERS];
static PortAutomapMarker *markers = home_markers;
static int marker_count;

static PortAutomapPlayer player;
static uint64_t polled_vblank;
static int last_reveal_cell = -1;
static float last_reveal_heading;
static int polls;
static int polls_since_build;

/* ---- Text ---------------------------------------------------------------------------------------- */
/* The game's text codes (font.txc, item_nam.prm): 0x08xx is the character xx + 0x20, 0x8000 a line break,
   0xFFFF the end; anything else (the Japanese glyphs, padding) is left out. */
static void decode(const u16 *codes, int count, char *out, size_t size) {
    size_t n = 0;
    int i, space = 0;

    for (i = 0; i < count && codes[i] != 0xFFFF && n + 1 < size; i++) {
        u16 c = codes[i];

        if ((c & 0xFF00) == 0x0800) {
            char ch = (char)((c & 0xFF) + 0x20);

            if (ch == ' ' && (n == 0 || space)) {
                continue;
            }
            out[n++] = ch;
            space = ch == ' ';
        } else if (c == 0x8000 && n > 0 && !space) {
            out[n++] = ' ';
            space = 1;
        }
    }
    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }
    out[n] = 0;
}

static void message_text(int id, char *out, size_t size) {
    sptr text;

    out[0] = 0;
    if (id <= 0 || id >= 0x1FFF) {
        return;
    }
    text = Message_GetText(id);
    if (text == 0) {
        return;
    }
    decode((const u16 *)text, 160, out, size);
}

static void item_name(int item, char *out, size_t size) {
    out[0] = 0;
    if (item > 0 && item < ITEM_COUNT) {
        decode((const u16 *)&item_name_defs[item], 0x30 / 2, out, size);
    }
    if (out[0] == 0) {
        snprintf(out, size, "item %d", item);
    }
}

/* What a map's markers are made from besides its objects and spawns, for the map in a slot (map_tables) or in a
   file (the browser) */
typedef struct {
    int tag;                        /* the map's number */
    const MapItemDef *items;        /* its fixed items (map_item_defs), item_count of them */
    int item_count;
    const s16 *item_kinds;          /* the item models it has (map_item_model_kinds: MapItem_FindModelSource) */
    const EntitySpawnInfo *spawns;  /* its spawns (map_spawn_infos), spawn_count of them */
    int spawn_count;
} MapTables;

static MapTables map_tables(int slot) {
    MapTables t;

    t.tag = map_slots[slot].tag;
    t.items = map_item_defs[slot];
    t.item_count = map_fixed_item_counts[slot];
    t.item_kinds = map_item_model_kinds[slot];
    t.spawns = map_spawn_infos[slot];
    t.spawn_count = map_spawn_counts[slot];
    return t;
}

/* a spawn's chr.prm record (Entity_InitFromMapSpawns) */
static const EntityParams *spawn_params(const EntitySpawnInfo *si) {
    return (const EntityParams *)(entity_params + si->type_id * sizeof(EntityParams));
}

/* What MapItem_PutOutForObject does with the item a map object holds (MapObjectUnk08.kind_param_0) in this
   map slot: 1 puts it out to be taken; 0 puts nothing out (not an item, or the map has no model for it:
   MapItem_FindModelSource); below 0, not now, for an item of a kind the player may have only one of: -1 he
   has it, -2 a shop has it in stock, -3 one lies dropped in a map (MapItem_UniqueNot*). */
static int item_put_out(const MapTables *t, int item) {
    int k;

    if (item <= 0 || item >= ITEM_COUNT || t->item_kinds == NULL) {
        return 0;
    }
    /* MapItem_FindModelSource: the map's item model kinds, up to the first below 0 */
    for (k = 0; k < 0x20 && t->item_kinds[k] >= 0 && t->item_kinds[k] != item_name_defs[item].model_file_number; k++) {
    }
    if (k == 0x20 || t->item_kinds[k] < 0) {
        return 0;
    }
    if (!MapItem_UniqueNotCarried(item)) {
        return -1;
    }
    if (!MapItem_UniqueNotInTradeStock(item)) {
        return -2;
    }
    if (!MapItem_UniqueNotDropped(item)) {
        return -3;
    }
    return 1;
}

/* the person's name: the last line of their page of the Recollection screen (record 100 + p, messages
   3000 + 50p onwards: decomp/docs/GAME_DATA.md "Characters"); a variant record that has no page of its own
   borrows its model's (chr.prm's model number) */
static void person_name(int record, int model, char *out, size_t size) {
    int tries[3], t;

    tries[0] = record;
    tries[1] = model;
    tries[2] = Automap_PersonPage(record); /* game/automap_guide.c */
    out[0] = 0;
    for (t = 0; t < 3 && out[0] == 0; t++) {
        if (tries[t] >= 100 && tries[t] < 100 + 99) {
            message_text(3000 + 50 * (tries[t] - 100) + 49, out, size);
        }
    }
    if (out[0] == 0) {
        snprintf(out, size, "Person");
    }
}

static void add_line(char *text, size_t size, const char *line) {
    size_t n = strlen(text);

    if (line == NULL || line[0] == 0) {
        return;
    }
    snprintf(text + n, size - n, "%s%s", n ? "\n" : "", line);
}

/* ---- The meshes ---------------------------------------------------------------------------------- */
typedef void (*TriangleFn)(const float a[3], const float b[3], const float c[3], int effect, void *context);

/* every triangle of strip list `list` of a model's collision mesh, in the world */
static void mesh_triangles(ModelInstance *model, int list, TriangleFn fn, void *context) {
    CollisionMesh *mesh = (CollisionMesh *)model->strips;
    const CollisionMeshLists *lists = (const CollisionMeshLists *)mesh;
    const f32 *m = (const f32 *)model->collision_matrix;
    const Vector4 *local;
    CollisionStrip *strip;
    int s, k;

    if (mesh == NULL || lists->strip_counts[list] == 0 || lists->strip_lists[list] == 0) {
        return;
    }
    local = EE_PTR(const Vector4 *, mesh->points);
    strip = EE_PTR(CollisionStrip *, lists->strip_lists[list]);
    for (s = 0; s < lists->strip_counts[list]; s++) {
        int n = strip->point_count;
        float p[3][3];

        for (k = 0; k < n; k++) {
            CollisionStripPoint *sp = &strip->points[k];
            s64 index = ((char *)EE_PTR(char *, sp->position) - (char *)COLLISION_WORLD_POINTS) / 16;
            const f32 *v;
            float *w = p[k % 3];

            if (index < 0 || (u64)index >= mesh->point_count) {
                index = 0;
            }
            v = &local[index].x;
            w[0] = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12];
            w[1] = v[0] * m[1] + v[1] * m[5] + v[2] * m[9] + m[13];
            w[2] = v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14];
            if (k >= 2) {
                CollisionStripPoint *first = &strip->points[k - 2];

                fn(p[(k - 2) % 3], p[(k - 1) % 3], p[k % 3],
                   (int)Collision_GetSurfaceEffect((u32)first->material_first, (u32)first->material_second,
                                                   first->material_third),
                   context);
            }
        }
        strip = (CollisionStrip *)&strip->points[n];
    }
}

/* the records of a slot that have collision meshes, list by list */
static void map_triangles(MapSlot *slot, int list, TriangleFn fn, void *context) {
    int count = ((MapSlotUnk08 *)slot->header)->record_count, i;

    for (i = 0; i < count; i++) {
        MapRecordBounds *record = (MapRecordBounds *)(slot->records + i * 0x80);
        ModelInstance *model = MAP_RECORD_MODEL(record);

        if (model == NULL || model->strips == NULL || (record->record_flags & 0x40000000)) {
            continue;
        }
        mesh_triangles(model, list, fn, context);
    }
}

/* ---- A map's file ---------------------------------------------------------------------------------
   The neighbours' floors (the ways into them) and the maps the browser shows are read from their files on the
   disc (KF4.DAT's NNN.map), laid out as Map_ParseFile reads them: the header (MapSlotUnk08), the records from
   0x17C0 (0x80 bytes each, MapRecordBounds), the flag blocks (0x40), the origins (32 of 16 bytes,
   MapLoadRequest), the map objects (0x80, MapObjectUnk08), the spawns (0x60, EntitySpawnInfo), the fixed items
   (0x40, MapItemDef), their model kinds (32 s16) and the groups' thresholds (16 bytes); the script at
   MapSlotUnk08.script_offset; the collision meshes back to back from MapSlotUnk08.record_collision_offset and
   object_collision_offset, each giving its size at 0x14 (CollisionMesh.mesh_size), its strip lists offsets from
   it, a strip point naming a point by number (Collision_RelocateMesh). */
typedef struct {
    uint8_t *data; /* malloc'd, whole sectors */
    uint32_t size;
    const MapSlotUnk08 *header;
    const uint8_t *records, *origins, *objects, *spawns, *items, *item_kinds, *thresholds;
    const u32 *script, *end;
    /* (the browser) the records left out (record_flags 0x40000000) and moved as the game has them now; NULL: as
       the file has them */
    u8 *record_hidden;
    float (*record_move)[3];
} MapFile;

static float file_f32(const uint8_t *p) {
    float f;

    memcpy(&f, p, 4);
    return f;
}

static uint32_t file_u32(const uint8_t *p) {
    uint32_t v;

    memcpy(&v, p, 4);
    return v;
}

static void map_file_free(MapFile *f) {
    free(f->data);
    free(f->record_hidden);
    free(f->record_move);
    memset(f, 0, sizeof(*f));
}

/* a map's file from the disc into *f (port_disc_read takes the disc's lock and seeks for each read, so it can
   come between the game's own reads); 0 when it is not there or not whole */
static int map_file_read(int map, MapFile *f) {
    const MapSlotUnk08 *h;
    char name[16];
    uint32_t lsn, at;

    memset(f, 0, sizeof(*f));
    snprintf(name, sizeof(name), "%03d.map", map);
    if (!port_disc_find_member(name, &lsn, &f->size) || f->size < 0x17C0 || f->size > 0x1000000) {
        return 0;
    }
    f->data = malloc(((size_t)f->size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE * DISC_SECTOR_SIZE);
    if (f->data == NULL || !port_disc_read(lsn, (f->size + DISC_SECTOR_SIZE - 1) / DISC_SECTOR_SIZE, f->data)) {
        map_file_free(f);
        return 0;
    }
    h = f->header = (const MapSlotUnk08 *)f->data;
    at = 0x17C0;
    f->records = f->data + at;
    at += h->record_count * 0x80u + h->flag_block_count * 0x40u;
    f->origins = f->data + at;
    at += 0x200;
    f->objects = f->data + at;
    at += h->object_count * 0x80u;
    f->spawns = f->data + at;
    at += h->spawn_count * 0x60u;
    f->items = f->data + at;
    at += h->item_count * 0x40u;
    f->item_kinds = f->data + at;
    f->thresholds = f->data + at + 0x40;
    if (at + 0x50 > f->size || (uint32_t)h->script_offset >= f->size || (h->script_offset & 3)) {
        map_file_free(f);
        return 0;
    }
    f->script = (const u32 *)(f->data + h->script_offset);
    f->end = (const u32 *)(f->data + (f->size & ~3u));
    return 1;
}

/* where mesh `index` of the meshes back to back from `first` starts, or 0 */
static uint32_t file_mesh(const MapFile *f, uint32_t first, int index) {
    uint32_t at = first;

    while (index-- > 0 && at + 0x50 <= f->size) {
        uint32_t size = file_u32(f->data + at + 0x14);

        if (size < 0x50) {
            return 0;
        }
        at += size;
    }
    return at + 0x50 <= f->size ? at : 0;
}

/* the matrix of a record (Map_PlaceRecordModels builds collision_matrix so: scale, then turns about x, y and z,
   then the place) or of a map object (ModelMatrix_Build: scale, turns about z, x and y, the place), from the
   position, turn and scale at p */
static void file_matrix(const uint8_t *p, int object, float m[16]) {
    float position[4], turn[3], scale[3];
    int k;

    for (k = 0; k < 3; k++) {
        position[k] = file_f32(p + 0x00 + 4 * k);
        turn[k] = file_f32(p + 0x10 + 4 * k);
        scale[k] = file_f32(p + 0x20 + 4 * k);
    }
    position[3] = 1.0f;
    sceVu0UnitMatrix(m);
    m[0] *= scale[0];
    m[5] *= scale[1];
    m[10] *= scale[2];
    if (object) {
        sceVu0RotMatrixZ(m, m, turn[2]);
        sceVu0RotMatrixX(m, m, turn[0]);
        sceVu0RotMatrixY(m, m, turn[1]);
    } else {
        sceVu0RotMatrixX(m, m, turn[0]);
        sceVu0RotMatrixY(m, m, turn[1]);
        sceVu0RotMatrixZ(m, m, turn[2]);
    }
    sceVu0TransMatrix(m, m, position);
}

/* every triangle of strip list `list` of the mesh at `at`, placed by m and moved by offset, as mesh_triangles */
static void file_mesh_triangles(const MapFile *f, uint32_t at, const float m[16], const float offset[3], int list,
                                TriangleFn fn, void *context) {
    const uint8_t *d = f->data;
    uint32_t strip_count = file_u32(d + at), point_count = file_u32(d + at + 0x0C), points, cursor = at + 0x50;
    int count = d[at + 0x38 + 2 * list] | d[at + 0x39 + 2 * list] << 8, s, k;

    for (k = 0; k < (int)strip_count && cursor + 0x20 <= f->size; k++) {
        cursor += 0x20 + (uint32_t)(d[cursor] | d[cursor + 1] << 8) * 16;
    }
    points = cursor;
    if ((uint64_t)points + (uint64_t)point_count * 16 > f->size || point_count == 0) {
        return;
    }
    cursor = at + file_u32(d + at + 0x18 + 4 * list);
    for (s = 0; s < count && cursor + 0x20 <= f->size; s++) {
        int n = d[cursor] | d[cursor + 1] << 8;
        float p[3][3];

        if (cursor + 0x20 + (uint32_t)n * 16 > f->size) {
            break;
        }
        for (k = 0; k < n; k++) {
            const uint8_t *point = d + cursor + 0x20 + 16 * k;
            uint32_t index = file_u32(point);
            const uint8_t *v = d + points + 16 * (index < point_count ? index : 0);
            float x = file_f32(v), y = file_f32(v + 4), z = file_f32(v + 8), *w = p[k % 3];

            w[0] = x * m[0] + y * m[4] + z * m[8] + m[12] + offset[0];
            w[1] = x * m[1] + y * m[5] + z * m[9] + m[13] + offset[1];
            w[2] = x * m[2] + y * m[6] + z * m[10] + m[14] + offset[2];
            if (k >= 2) {
                const uint8_t *first = point - 32;

                fn(p[(k - 2) % 3], p[(k - 1) % 3], p[k % 3],
                   (int)Collision_GetSurfaceEffect(file_u32(first + 4), file_u32(first + 8), (int)file_u32(first + 12)), context);
            }
        }
        cursor += 0x20 + (uint32_t)n * 16;
    }
}

/* the records of a map file that have collision meshes (as map_triangles), moved by offset: the strip lists
   whose bits `lists` has, record by record */
static void file_triangles(const MapFile *f, const float offset[3], int lists, TriangleFn fn, void *context) {
    int r, list;

    for (r = 0; r < f->header->record_count; r++) {
        const uint8_t *rec = f->records + r * 0x80;
        int mesh = rec[0x32] | rec[0x33] << 8;
        uint32_t at;
        float m[16];

        if (mesh == 0xFFFF ||
            (f->record_hidden != NULL ? f->record_hidden[r] : (file_u32(rec + 0x3C) & 0x40000000) != 0) ||
            (at = file_mesh(f, (uint32_t)f->header->record_collision_offset, mesh)) == 0) {
            continue;
        }
        file_matrix(rec, 0, m);
        if (f->record_move != NULL) {
            m[12] += f->record_move[r][0];
            m[13] += f->record_move[r][1];
            m[14] += f->record_move[r][2];
        }
        for (list = 0; list < 6; list++) {
            if (lists & (1 << list)) {
                file_mesh_triangles(f, at, m, offset, list, fn, context);
            }
        }
    }
}

/* What a plan is built from: a map slot's records (the map the player is in), or a map's file (one browsed) */
typedef struct {
    int slot;
    const MapFile *file;
} PlanSource;

static void source_triangles(const PlanSource *source, int list, TriangleFn fn, void *context) {
    static const float none[3] = {0.0f, 0.0f, 0.0f};

    if (source->file != NULL) {
        file_triangles(source->file, none, 1 << list, fn, context);
    } else {
        map_triangles(&map_slots[source->slot], list, fn, context);
    }
}

/* ---- The floor plan ------------------------------------------------------------------------------ */
typedef struct {
    float x0, z0, x1, z1;
    int any;
} Bounds;

static void grow_bounds(const float a[3], const float b[3], const float c[3], int effect, void *context) {
    Bounds *bounds = context;
    const float *p[3] = {a, b, c};
    int i;

    (void)effect;
    for (i = 0; i < 3; i++) {
        if (!bounds->any) {
            bounds->x0 = bounds->x1 = p[i][0];
            bounds->z0 = bounds->z1 = p[i][2];
            bounds->any = 1;
        }
        bounds->x0 = p[i][0] < bounds->x0 ? p[i][0] : bounds->x0;
        bounds->x1 = p[i][0] > bounds->x1 ? p[i][0] : bounds->x1;
        bounds->z0 = p[i][2] < bounds->z0 ? p[i][2] : bounds->z0;
        bounds->z1 = p[i][2] > bounds->z1 ? p[i][2] : bounds->z1;
    }
}

static PortAutomapLayer *cell_layers(int i, int j) {
    return &layers[((size_t)j * plan.width + i) * LAYERS];
}

typedef void (*CellFn)(int i, int j, float y, void *context);

/* calls fn for every cell whose centre the triangle covers (top down), with the triangle's height there */
static void raster(const float a[3], const float b[3], const float c[3], CellFn fn, void *context) {
    float det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
    float lo_x = fminf(a[0], fminf(b[0], c[0])), hi_x = fmaxf(a[0], fmaxf(b[0], c[0]));
    float lo_z = fminf(a[2], fminf(b[2], c[2])), hi_z = fmaxf(a[2], fmaxf(b[2], c[2]));
    int i0, i1, j0, j1, i, j;

    if (fabsf(det) < 1e-3f) {
        return;
    }
    i0 = (int)floorf((lo_x - plan.x0) / CELL - 0.5f);
    i1 = (int)ceilf((hi_x - plan.x0) / CELL - 0.5f);
    j0 = (int)floorf((lo_z - plan.z0) / CELL - 0.5f);
    j1 = (int)ceilf((hi_z - plan.z0) / CELL - 0.5f);
    i0 = i0 < 0 ? 0 : i0;
    j0 = j0 < 0 ? 0 : j0;
    i1 = i1 >= plan.width ? plan.width - 1 : i1;
    j1 = j1 >= plan.height ? plan.height - 1 : j1;
    for (j = j0; j <= j1; j++) {
        float pz = plan.z0 + ((float)j + 0.5f) * CELL;

        for (i = i0; i <= i1; i++) {
            float px = plan.x0 + ((float)i + 0.5f) * CELL;
            float u = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / det;
            float v = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / det;

            if (u < -1e-3f || v < -1e-3f || u + v > 1.001f) {
                continue;
            }
            fn(i, j, u * a[1] + v * b[1] + (1.0f - u - v) * c[1], context);
        }
    }
}

/* what wins when two surfaces are one layer: the floor is the least of them */
static int priority(int kind) {
    switch (kind) {
    case PORT_AUTOMAP_FLOOR: return 1;
    case PORT_AUTOMAP_WATER: return 2;
    case PORT_AUTOMAP_POISON: return 3;
    case PORT_AUTOMAP_HURT: return 4;
    case PORT_AUTOMAP_FIRE: return 5;
    case PORT_AUTOMAP_DEATH: return 6;
    default: return 0;
    }
}

static void add_layer(int i, int j, float yf, void *context) {
    PortAutomapLayer *l = cell_layers(i, j);
    int kind = *(const int *)context, y = (int)yf, n, k;

    if (y <= -32000 || y >= 32000) {
        return;
    }
    for (n = 0; n < LAYERS && l[n].y != NO_Y; n++) {
        if (abs(l[n].y - y) < MERGE) {
            if (priority(kind) > priority(l[n].kind)) {
                l[n].kind = (uint8_t)kind;
            }
            if (kind != PORT_AUTOMAP_DEATH && y < l[n].y) {
                l[n].y = (int16_t)y;
            }
            return;
        }
    }
    /* sorted from the highest (the least y); when full, the lowest goes */
    for (n = 0; n < LAYERS && l[n].y != NO_Y && l[n].y < y; n++) {
    }
    if (n == LAYERS) {
        return;
    }
    for (k = LAYERS - 1; k > n; k--) {
        l[k] = l[k - 1];
    }
    l[n].y = (int16_t)y;
    l[n].kind = (uint8_t)kind;
    l[n].flags = 0;
}

static void floor_triangle(const float a[3], const float b[3], const float c[3], int effect, void *context) {
    int kind = *(const int *)context;

    if (kind == PORT_AUTOMAP_POISON) { /* the hazards: which one by the effect */
        kind = effect == 0                ? PORT_AUTOMAP_DEATH
               : effect >= 1 && effect <= 3 ? PORT_AUTOMAP_POISON
               : effect == 4                ? PORT_AUTOMAP_FIRE /* lava, on every map that has it: game/automap_guide.c */
                                            : PORT_AUTOMAP_HURT;
    }
    raster(a, b, c, add_layer, &kind);
}

/* a wall from y lo (its top) to hi (its bottom) in cell (i, j) */
static void stamp_wall(int i, int j, int lo, int hi) {
    PortAutomapLayer *l;
    int n, any = 0;
    size_t cell;

    if (i < 0 || j < 0 || i >= plan.width || j >= plan.height) {
        return;
    }
    l = cell_layers(i, j);
    for (n = 0; n < LAYERS && l[n].y != NO_Y; n++) {
        if (l[n].kind == PORT_AUTOMAP_DEATH) {
            continue;
        }
        any = 1;
        if (lo < l[n].y - WALL_LOW && hi > l[n].y - WALL_HIGH) {
            l[n].flags |= PORT_AUTOMAP_WALL;
        }
    }
    if (any) {
        return;
    }
    cell = (size_t)j * plan.width + i;
    if (solid_top[cell] == NO_Y) {
        solid_top[cell] = (int16_t)lo;
        solid_bottom[cell] = (int16_t)hi;
    } else {
        solid_top[cell] = (int16_t)(lo < solid_top[cell] ? lo : solid_top[cell]);
        solid_bottom[cell] = (int16_t)(hi > solid_bottom[cell] ? hi : solid_bottom[cell]);
    }
}

static void wall_cell(int i, int j, float y, void *context) {
    (void)context;
    stamp_wall(i, j, (int)y, (int)y);
}

static void wall_triangle(const float a[3], const float b[3], const float c[3], int effect, void *context) {
    float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    const float *p[3] = {a, b, c};
    int e;

    (void)effect;
    (void)context;
    if (length < 1e-3f) {
        return;
    }
    if (fabsf(n[1]) / length >= STEEP) {
        raster(a, b, c, wall_cell, NULL);
        return;
    }
    {
        int lo = (int)fminf(a[1], fminf(b[1], c[1])), hi = (int)fmaxf(a[1], fmaxf(b[1], c[1]));

        for (e = 0; e < 3; e++) {
            const float *q0 = p[e], *q1 = p[(e + 1) % 3];
            float dx = q1[0] - q0[0], dz = q1[2] - q0[2];
            int steps = (int)(fmaxf(fabsf(dx), fabsf(dz)) / (CELL * 0.5f)) + 1, s;

            for (s = 0; s <= steps; s++) {
                float t = (float)s / (float)steps;

                stamp_wall((int)floorf((q0[0] + dx * t - plan.x0) / CELL), (int)floorf((q0[2] + dz * t - plan.z0) / CELL),
                           lo, hi);
            }
        }
    }
}

static uint32_t fnv(uint32_t h, uint32_t v) {
    return (h ^ v) * 16777619u;
}

/* DEATH layers more than FAR_CELLS from any other layer are the void around the map: not drawn */
static void mark_far(void) {
    int w = plan.width, h = plan.height, i, j, n;
    uint8_t *d = malloc((size_t)w * h);

    if (d == NULL) {
        return;
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            const PortAutomapLayer *l = cell_layers(i, j);
            int near = 0;

            for (n = 0; n < LAYERS && l[n].y != NO_Y; n++) {
                near |= l[n].kind != PORT_AUTOMAP_DEATH;
            }
            d[(size_t)j * w + i] = near ? 0 : 255;
        }
    }
    /* a chamfer distance, two passes */
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            uint8_t *c = &d[(size_t)j * w + i];
            if (i > 0 && c[-1] + 1 < *c) *c = (uint8_t)(c[-1] + 1);
            if (j > 0 && c[-w] + 1 < *c) *c = (uint8_t)(c[-w] + 1);
        }
    }
    for (j = h - 1; j >= 0; j--) {
        for (i = w - 1; i >= 0; i--) {
            uint8_t *c = &d[(size_t)j * w + i];
            if (i < w - 1 && c[1] + 1 < *c) *c = (uint8_t)(c[1] + 1);
            if (j < h - 1 && c[w] + 1 < *c) *c = (uint8_t)(c[w] + 1);
        }
    }
    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            PortAutomapLayer *l = cell_layers(i, j);

            if (d[(size_t)j * w + i] > FAR_CELLS) {
                for (n = 0; n < LAYERS && l[n].y != NO_Y; n++) {
                    l[n].flags |= PORT_AUTOMAP_FAR;
                }
            }
        }
    }
    free(d);
}

static void apply_stored(void);
static void find_exits(const char *records, int count, const char *origins);
static void scan_script(const u32 *cursor, const u32 *end);
static int script_gift(int label, int depth);
static int script_message(int label);

/* the floor plan of map `map` from `source` into the plan in hand, with the seen state stored for it (the
   ways and the script's links are the caller's) */
static void build_plan(const PlanSource *source, int map, float x, float z) {
    Bounds bounds = {0};
    size_t cells;
    int kind, i;

    plan.map = map;
    source_triangles(source, 1, grow_bounds, &bounds);
    source_triangles(source, 5, grow_bounds, &bounds);
    if (!bounds.any) {
        bounds.x0 = bounds.x1 = x;
        bounds.z0 = bounds.z1 = z;
    }
    /* the corner on whole cells of the world, so that cells mean the same in every build */
    plan.x0 = floorf((bounds.x0 - MARGIN) / CELL) * CELL;
    plan.z0 = floorf((bounds.z0 - MARGIN) / CELL) * CELL;
    plan.width = (int)ceilf((bounds.x1 + MARGIN - plan.x0) / CELL);
    plan.height = (int)ceilf((bounds.z1 + MARGIN - plan.z0) / CELL);
    plan.width = plan.width > MAX_CELLS ? MAX_CELLS : plan.width < 1 ? 1 : plan.width;
    plan.height = plan.height > MAX_CELLS ? MAX_CELLS : plan.height < 1 ? 1 : plan.height;
    cells = (size_t)plan.width * plan.height;
    free(layers);
    free(solid_top);
    free(solid_bottom);
    free(seen);
    layers = malloc(cells * LAYERS * sizeof(*layers));
    solid_top = malloc(cells * sizeof(*solid_top));
    solid_bottom = malloc(cells * sizeof(*solid_bottom));
    seen = calloc(cells, 1);
    if (layers == NULL || solid_top == NULL || solid_bottom == NULL || seen == NULL) {
        plan.map = -1;
        return;
    }
    for (i = 0; i < (int)(cells * LAYERS); i++) {
        layers[i].y = NO_Y;
        layers[i].kind = PORT_AUTOMAP_NONE;
        layers[i].flags = 0;
    }
    for (i = 0; i < (int)cells; i++) {
        solid_top[i] = solid_bottom[i] = NO_Y;
    }
    kind = PORT_AUTOMAP_FLOOR;
    source_triangles(source, 1, floor_triangle, &kind);
    kind = PORT_AUTOMAP_WATER;
    source_triangles(source, 5, floor_triangle, &kind);
    kind = PORT_AUTOMAP_POISON; /* the hazards, sorted by effect */
    source_triangles(source, 4, floor_triangle, &kind);
    source_triangles(source, 0, wall_triangle, NULL);
    source_triangles(source, 2, wall_triangle, NULL);
    mark_far();

    layout_hash = 2166136261u;
    for (i = 0; i < (int)(cells * LAYERS); i++) {
        if (layers[i].y != NO_Y) {
            layout_hash = fnv(fnv(layout_hash, (uint32_t)i), (uint32_t)(layers[i].y / 64) * 8u + layers[i].kind);
        }
    }
    plan.layers = layers;
    plan.solid_top = solid_top;
    plan.solid_bottom = solid_bottom;
    plan.seen = seen;
    plan.version++;
    apply_stored();
}

static void build(int slot_index) {
    MapSlot *slot = &map_slots[slot_index];
    PlanSource source = {slot_index, NULL};

    built_slot = slot_index;
    built_records = slot->records;
    build_plan(&source, slot->tag, player_state.position.x, player_state.position.z);
    if (plan.map < 0) {
        return;
    }
    last_reveal_cell = -1;
    find_exits(slot->records, ((MapSlotUnk08 *)slot->header)->record_count, (const char *)slot->origins);
    scan_script((const u32 *)slot->script, NULL);
}

/* ---- What has been seen, stored per map ----------------------------------------------------------
   A map's chunk: map, the corner cell (x0 / CELL, z0 / CELL), width, height, the layout's hash, the number of
   runs, then the runs: a first cell, a length and the mask of seen layers, for each stretch of cells with the
   same mask. A chunk read into a plan of another layout (a later build that cuts layers differently) marks
   every layer of a cell it names as seen. */
#define CHUNK_HEAD 28

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static StoredMap *find_stored(int map, int make) {
    int i;

    for (i = 0; i < stored_count; i++) {
        if (stored[i].map == map) {
            return &stored[i];
        }
    }
    if (!make || stored_count == MAX_MAPS) {
        return NULL;
    }
    stored[stored_count].map = map;
    stored[stored_count].data = NULL;
    stored[stored_count].size = 0;
    return &stored[stored_count++];
}

/* whether the player has explored some of the map: its chunk is stored (it is written as he leaves the map) */
static int map_explored(int map) {
    const StoredMap *s = find_stored(map, 0);

    return s != NULL && s->size >= CHUNK_HEAD && get32(s->data + 24) > 0;
}

/* the current plan's seen state into its chunk */
void Automap_Flush(void) {
    size_t cells, runs = 0, c;
    StoredMap *s;
    uint8_t *data, *p;

    if (plan.map < 0 || seen == NULL) {
        return;
    }
    cells = (size_t)plan.width * plan.height;
    for (c = 0; c < cells;) {
        size_t start = c;

        while (c < cells && seen[c] == seen[start] && c - start < 0xFFFF) {
            c++;
        }
        runs += seen[start] != 0;
    }
    data = malloc(CHUNK_HEAD + runs * 8);
    s = find_stored(plan.map, 1);
    if (data == NULL || s == NULL) {
        free(data);
        return;
    }
    put32(data, (uint32_t)plan.map);
    put32(data + 4, (uint32_t)(int32_t)floorf(plan.x0 / CELL + 0.5f));
    put32(data + 8, (uint32_t)(int32_t)floorf(plan.z0 / CELL + 0.5f));
    put32(data + 12, (uint32_t)plan.width);
    put32(data + 16, (uint32_t)plan.height);
    put32(data + 20, layout_hash);
    put32(data + 24, (uint32_t)runs);
    p = data + CHUNK_HEAD;
    for (c = 0; c < cells;) {
        size_t start = c;

        while (c < cells && seen[c] == seen[start] && c - start < 0xFFFF) {
            c++;
        }
        if (seen[start] != 0) {
            put32(p, (uint32_t)start);
            p[4] = (uint8_t)(c - start);
            p[5] = (uint8_t)((c - start) >> 8);
            p[6] = seen[start];
            p[7] = 0;
            p += 8;
        }
    }
    free(s->data);
    s->data = data;
    s->size = CHUNK_HEAD + runs * 8;
}

/* the stored chunk of the plan's map into its seen bits */
static void apply_stored(void) {
    StoredMap *s = find_stored(plan.map, 0);
    int32_t cx0, cz0, my_cx0 = (int32_t)floorf(plan.x0 / CELL + 0.5f), my_cz0 = (int32_t)floorf(plan.z0 / CELL + 0.5f);
    uint32_t w, h, hash, runs, r;
    int same;

    if (s == NULL || s->size < CHUNK_HEAD) {
        return;
    }
    cx0 = (int32_t)get32(s->data + 4);
    cz0 = (int32_t)get32(s->data + 8);
    w = get32(s->data + 12);
    h = get32(s->data + 16);
    hash = get32(s->data + 20);
    runs = get32(s->data + 24);
    same = cx0 == my_cx0 && cz0 == my_cz0 && w == (uint32_t)plan.width && h == (uint32_t)plan.height && hash == layout_hash;
    for (r = 0; r < runs && CHUNK_HEAD + (r + 1) * 8 <= s->size; r++) {
        const uint8_t *p = s->data + CHUNK_HEAD + r * 8;
        uint32_t start = get32(p), length = p[4] | (uint32_t)p[5] << 8, k;
        uint8_t mask = same ? p[6] : 0xF;

        for (k = 0; k < length; k++) {
            uint32_t cell = start + k;
            int i = (int)(cell % w) + cx0 - my_cx0, j = (int)(cell / w) + cz0 - my_cz0;

            if (w == 0 || i < 0 || j < 0 || i >= plan.width || j >= plan.height) {
                continue;
            }
            seen[(size_t)j * plan.width + i] |= mask;
        }
    }
    plan.version++;
}

size_t Automap_Serialize(uint8_t **out) {
    size_t size = 16, at;
    uint8_t *data;
    int i;

    for (i = 0; i < stored_count; i++) {
        size += 4 + stored[i].size;
    }
    data = malloc(size);
    *out = data;
    if (data == NULL) {
        return 0;
    }
    memcpy(data, "KF4AMAP", 8);
    put32(data + 8, 1);
    put32(data + 12, (uint32_t)stored_count);
    at = 16;
    for (i = 0; i < stored_count; i++) {
        put32(data + at, (uint32_t)stored[i].size);
        memcpy(data + at + 4, stored[i].data, stored[i].size);
        at += 4 + stored[i].size;
    }
    return size;
}

void Automap_Deserialize(const uint8_t *data, size_t size) {
    uint32_t count, i;
    size_t at = 16;

    for (i = 0; i < (uint32_t)stored_count; i++) {
        free(stored[i].data);
    }
    stored_count = 0;
    if (data != NULL && size >= 16 && memcmp(data, "KF4AMAP", 8) == 0 && get32(data + 8) == 1) {
        count = get32(data + 12);
        for (i = 0; i < count && at + 4 <= size; i++) {
            uint32_t length = get32(data + at);
            StoredMap *s;

            if (length < CHUNK_HEAD || at + 4 + length > size) {
                break;
            }
            s = find_stored((int)get32(data + at + 4), 1);
            if (s != NULL) {
                free(s->data);
                s->data = malloc(length);
                if (s->data != NULL) {
                    memcpy(s->data, data + at + 4, length);
                    s->size = length;
                }
            }
            at += 4 + length;
        }
    }
    /* the plan is built again, from what was read, without flushing the old state into it */
    if (plan.map >= 0 && seen != NULL) {
        memset(seen, 0, (size_t)plan.width * plan.height);
        apply_stored();
    }
}

/* ---- In a save state (port/platform/savestate.c) ---------------------------------------------------
   This file is linked after the state's image (CMakeLists.txt, game_host): its globals hold malloc'd
   buffers, which a state cannot carry. A state holds the fog of war as Automap_Serialize writes it
   instead, and what is built from the game's maps is built again from the game the state brings back. A
   state taken with the mod off holds none, and loading it leaves the fog of war as it is. */
void Automap_State(PortStateIo *io) {
    uint32_t kept = 0, size = 0;
    uint8_t *data = NULL;

    if (!io->loading && port_automap.enabled) {
        port_automap_start(); /* (live.bin first, if the mod has not run yet) */
        Automap_Flush();
        size = (uint32_t)Automap_Serialize(&data);
        kept = data != NULL;
    }
    PORT_STATE(io, kept);
    PORT_STATE(io, size);
    if (io->loading && size > 0) {
        data = malloc(size); /* (NULL: skipped) */
    }
    port_state_bytes(io, data, size);
    if (io->loading) {
        if (kept && data != NULL && port_automap.enabled) {
            port_automap_start(); /* (so that live.bin is not read over it later) */
            Automap_Deserialize(data, size);
            port_automap_state_loaded();
        }
        /* game RAM is another: the plan, its exits and the scripts read are built again at the next poll */
        built_slot = -1;
        built_records = NULL;
        last_reveal_cell = -1;
        polls_since_build = 0;
    }
    free(data);
}

/* ---- Revealing ------------------------------------------------------------------------------------ */
static int cell_of(float x, float z, int *i, int *j) {
    *i = (int)floorf((x - plan.x0) / CELL);
    *j = (int)floorf((z - plan.z0) / CELL);
    return *i >= 0 && *j >= 0 && *i < plan.width && *j < plan.height;
}

/* the layer of cell (i, j) within `reach` of height y, nearest it: -1 none */
static int layer_near(int i, int j, int y, int reach) {
    const PortAutomapLayer *l = cell_layers(i, j);
    int n, best = -1, best_d = reach + 1;

    for (n = 0; n < LAYERS && l[n].y != NO_Y; n++) {
        int d = abs(l[n].y - y);

        if (d < best_d && l[n].kind != PORT_AUTOMAP_DEATH) {
            best = n;
            best_d = d;
        }
    }
    return best;
}

static int reveal_bit(int i, int j, int n) {
    uint8_t *s = &seen[(size_t)j * plan.width + i];

    if (*s & (1u << n)) {
        return 0;
    }
    *s |= (uint8_t)(1u << n);
    return 1;
}

static int reveal(void) {
    int pi, pj, start, changed = 0, r, di, dj;
    int y0;

    if (!cell_of(player_state.position.x, player_state.position.z, &pi, &pj)) {
        return 0;
    }
    start = layer_near(pi, pj, (int)player_state.position.y, 400);
    if (start < 0) {
        return 0;
    }
    y0 = cell_layers(pi, pj)[start].y;
    for (dj = -1; dj <= 1; dj++) {
        for (di = -1; di <= 1; di++) {
            int i = pi + di, j = pj + dj, n;

            if (i >= 0 && j >= 0 && i < plan.width && j < plan.height && (n = layer_near(i, j, y0, CLIMB)) >= 0) {
                changed |= reveal_bit(i, j, n);
            }
        }
    }
    for (r = 0; r < RAYS; r++) {
        float angle = (float)r * 6.2831853f / RAYS;
        float dx = cosf(angle), dz = sinf(angle);
        float ox = (player_state.position.x - plan.x0) / CELL, oz = (player_state.position.z - plan.z0) / CELL;
        int i = pi, j = pj, step_i = dx >= 0 ? 1 : -1, step_j = dz >= 0 ? 1 : -1;
        float td_i = fabsf(dx) < 1e-6f ? 1e30f : fabsf(1.0f / dx), td_j = fabsf(dz) < 1e-6f ? 1e30f : fabsf(1.0f / dz);
        float t_i = fabsf(dx) < 1e-6f ? 1e30f : ((dx > 0 ? (float)(i + 1) - ox : ox - (float)i) * td_i);
        float t_j = fabsf(dz) < 1e-6f ? 1e30f : ((dz > 0 ? (float)(j + 1) - oz : oz - (float)j) * td_j);
        float reach = REACH / CELL;
        int y = y0, air = 0;

        while (fminf(t_i, t_j) < reach) {
            const PortAutomapLayer *l;
            int n, below = -1;
            size_t cell;

            if (t_i < t_j) {
                i += step_i;
                t_i += td_i;
            } else {
                j += step_j;
                t_j += td_j;
            }
            if (i < 0 || j < 0 || i >= plan.width || j >= plan.height) {
                break;
            }
            l = cell_layers(i, j);
            cell = (size_t)j * plan.width + i;
            n = layer_near(i, j, y, CLIMB);
            if (n >= 0) {
                /* the floor goes on (or, after a gap, starts again at the height the ray goes) */
                changed |= reveal_bit(i, j, n);
                if (l[n].flags & PORT_AUTOMAP_WALL) {
                    break;
                }
                if (air == 0) {
                    y = l[n].y;
                }
                air = 0;
                continue;
            }
            /* a wall with no floor under it, at the height the ray goes */
            if (solid_top[cell] != NO_Y && solid_top[cell] < y - WALL_LOW && solid_bottom[cell] > y - WALL_HIGH) {
                break;
            }
            /* off a ledge: the floor below is seen; the ray goes on at its height, over it */
            for (n = 0; n < LAYERS && l[n].y != NO_Y; n++) {
                if (l[n].y > y && below < 0) {
                    below = n;
                }
            }
            if (below >= 0) {
                changed |= reveal_bit(i, j, below);
            }
            /* over a drop, or nothing at all (the outside): not far */
            if (++air > AIR_CELLS) {
                break;
            }
        }
    }
    return changed;
}

/* the cell the player stands on is floor he has walked */
static void walked(void) {
    int i, j, n;

    if (!cell_of(player_state.position.x, player_state.position.z, &i, &j)) {
        return;
    }
    n = layer_near(i, j, (int)player_state.position.y, 120);
    if (n >= 0) {
        PortAutomapLayer *l = cell_layers(i, j);

        if (!(l[n].flags & PORT_AUTOMAP_WALKED)) {
            l[n].flags |= PORT_AUTOMAP_WALKED;
            plan.version++;
        }
    }
}

/* ---- Markers --------------------------------------------------------------------------------------- */
static int seen_near(float x, float y, float z) {
    int i, j, di, dj;

    if (port_automap.reveal) {
        return 1;
    }
    if (!cell_of(x, z, &i, &j)) {
        return 0;
    }
    for (dj = -2; dj <= 2; dj++) {
        for (di = -2; di <= 2; di++) {
            int ci = i + di, cj = j + dj, n;

            if (ci < 0 || cj < 0 || ci >= plan.width || cj >= plan.height) {
                continue;
            }
            n = layer_near(ci, cj, (int)y, 600);
            if (n >= 0 && (seen[(size_t)cj * plan.width + ci] & (1u << n))) {
                return 1;
            }
        }
    }
    return 0;
}

static PortAutomapMarker *new_marker(int kind, float x, float y, float z) {
    PortAutomapMarker *m;

    if (marker_count == MAX_MARKERS) {
        return NULL;
    }
    m = &markers[marker_count++];
    memset(m, 0, sizeof(*m));
    m->kind = kind;
    m->x = x;
    m->y = y;
    m->z = z;
    return m;
}

static void lock_line(char *text, size_t size, int key) {
    char name[64], line[128];

    item_name(key, name, sizeof(name));
    snprintf(line, sizeof(line), "Needs: %s%s", name, item_counts[key] ? " (you have it)" : "");
    add_line(text, size, line);
}

/* the item of the map's fixed item with this id (MapItemDef.slot_id), or 0; *taken: it has been picked up as
   often as it may be (MapItem_PlaceFixed: map_item_pickup_counts against MapItemDef.required_flag) */
static int fixed_item(const MapTables *t, int id, int *taken) {
    int k;

    *taken = 0;
    for (k = 0; id != 0 && t->items != NULL && k < t->item_count && k < 0x40; k++) {
        const MapItemDef *def = &t->items[k];

        if (def->slot_id == id) {
            *taken = def->required_flag != 0 && map_item_pickup_counts[t->tag & 0x7F][k] >= def->required_flag;
            return def->item;
        }
    }
    return 0;
}

/* the contents line: the item (put_out from item_put_out: below 0, it stays where it is for now), else the gold,
   else "Empty" */
static void holds_line(char *text, size_t size, int item, int gold, int put_out) {
    static const char *const stays[] = {"", " (stays: you have one)", " (stays: a shop has one)", " (stays: one lies dropped)"};
    char name[64], line[128];

    if (item > 0) {
        item_name(item, name, sizeof(name));
        snprintf(line, sizeof(line), "Holds: %s%s", name, put_out < 0 && put_out >= -3 ? stays[-put_out] : "");
    } else if (gold > 0) {
        snprintf(line, sizeof(line), "Holds: %d gold", gold);
    } else {
        snprintf(line, sizeof(line), "Empty");
    }
    add_line(text, size, line);
}

/* a door's leaf, top down: the two farthest apart points of its collision mesh, in the world */
static int door_leaf(ModelInstance *model, float *x0, float *z0, float *x1, float *z1) {
    CollisionMesh *mesh;
    const Vector4 *local;
    const f32 *m;
    float px[64], pz[64], best = -1.0f;
    u32 n, a, b;

    if (model == NULL || model->strips == NULL) {
        return 0;
    }
    mesh = (CollisionMesh *)model->strips;
    local = EE_PTR(const Vector4 *, mesh->points);
    m = (const f32 *)model->collision_matrix;
    n = mesh->point_count < 64 ? mesh->point_count : 64;
    for (a = 0; a < n; a++) {
        const f32 *v = &local[a].x;

        px[a] = v[0] * m[0] + v[1] * m[4] + v[2] * m[8] + m[12];
        pz[a] = v[0] * m[2] + v[1] * m[6] + v[2] * m[10] + m[14];
    }
    for (a = 0; a < n; a++) {
        for (b = a + 1; b < n; b++) {
            float d = (px[a] - px[b]) * (px[a] - px[b]) + (pz[a] - pz[b]) * (pz[a] - pz[b]);

            if (d > best) {
                best = d;
                *x0 = px[a];
                *z0 = pz[a];
                *x1 = px[b];
                *z1 = pz[b];
            }
        }
    }
    return best > 100.0f * 100.0f && best < 3000.0f * 3000.0f;
}

/* The contents of a chest, bones, a grave or a container, and the ring: `item` the item it puts out
   (put_out from item_put_out; 0: none, or nothing the game would put out), else `gold`; and what its use
   script hands over, a fixed item (command 101: the gravestone of the Icon of Healing on map 097, whose
   kind_param_0, 1026, is the message that script shows, not an item), and (`say`) the message that script shows.
   A use script (MapObjectUnk08.use_script) takes the player's press before the object's own update sees it
   (Interact_FindTarget runs before MapObject_UpdateAll). Returns whether it holds something to take now. */
static int contents(PortAutomapMarker *m, const MapTables *t, const MapObjectUnk08 *src, const char *name, int item,
                    int put_out, int gold, int emptied, int say) {
    int gift_taken, gift = fixed_item(t, script_gift(src->use_script, 0), &gift_taken), gift_left = 0, said_id, holds;
    char text[160], line[176];

    said_id = script_message(src->use_script);
    message_text(said_id, text, sizeof(text));
    if (say && text[0] && strcmp(text, name) != 0) {
        snprintf(line, sizeof(line), "\"%s\"", text);
        add_line(m->text, sizeof(m->text), line);
    }
    holds = !emptied && (item != 0 ? put_out > 0 : gold > 0);
    if (gift > 0) {
        /* taken: the player has his one of it (or it is in a shop's stock or lies dropped somewhere). No ring:
           the script gives it only when the story's flags say so (the smith's work in the Earth Folk's chests
           of maps 017-019 once it is made; the Icon of Healing once, and it also lies on the grave) */
        gift_left = !gift_taken && item_put_out(t, gift) > 0;
    }
    m->flags |= holds ? PORT_AUTOMAP_MARK_HOLDS : 0;
    if (!port_automap.items) {
        return holds;
    }
    if (item != 0) {
        holds_line(m->text, sizeof(m->text), item, 0, emptied ? 1 : put_out);
        if (emptied) {
            add_line(m->text, sizeof(m->text), "(taken)");
        }
    } else if (gold > 0 && !emptied) {
        holds_line(m->text, sizeof(m->text), 0, gold, 0);
    }
    if (gift > 0) {
        item_name(gift, text, sizeof(text));
        snprintf(line, sizeof(line), "Gives (by its script): %s%s", text, gift_left ? "" : " (taken)");
        add_line(m->text, sizeof(m->text), line);
    }
    if (item == 0 && (gold <= 0 || emptied) && gift <= 0 && !emptied) {
        add_line(m->text, sizeof(m->text), "Empty");
    }
    return holds;
}

/* the marker of map object i: its definition and placement, its state (MapObject.state_flags), where it is, and
   its door's leaf when it is a door whose leaf is known (x0, z0, x1, z1) */
static void object_marker(const MapTables *t, int i, const MapObjectUnk04 *def, const MapObjectUnk08 *src, u32 state,
                          float x, float y, float z, const float *leaf) {
    const MapObjectUnk08Words *words = (const MapObjectUnk08Words *)src;
    PortAutomapMarker *m;
    int kind = def->kind, mark, cheat = 0, done = (state & MAP_OBJECT_ON) != 0, emptied = (state & MAP_OBJECT_EMPTIED) != 0;
    int name_id, put_out;
    char name[64];
    const char *hint;

    switch (kind) {
    case 1: case 2: case 6: case 17:
        mark = words->kind_param_0 != 0 && !emptied ? PORT_AUTOMAP_MARK_LOCKED_DOOR : PORT_AUTOMAP_MARK_DOOR;
        break;
    case 3: mark = PORT_AUTOMAP_MARK_REMAINS; break;
    case 4: mark = PORT_AUTOMAP_MARK_CHEST; break;
    case 7: case 8: case 30: mark = PORT_AUTOMAP_MARK_SWITCH; break;
    case 11: mark = PORT_AUTOMAP_MARK_EXAMINE; break;
    case 12: mark = PORT_AUTOMAP_MARK_PASSAGE; break;
    case 13: case 19: mark = PORT_AUTOMAP_MARK_PEDESTAL; break;
    case 14: mark = PORT_AUTOMAP_MARK_LIFT; break;
    case 15: mark = PORT_AUTOMAP_MARK_LADDER; break;
    case 16: case 18: mark = PORT_AUTOMAP_MARK_TRAP; cheat = 1; break;
    case 20: mark = PORT_AUTOMAP_MARK_SAVE; break;
    case 21: mark = PORT_AUTOMAP_MARK_CONTAINER; break;
    case 31: mark = PORT_AUTOMAP_MARK_SECRET; cheat = 1; break;
    default: return;
    }
    if (cheat && !port_automap.secrets) {
        return;
    }
    if (!cheat && !seen_near(x, y, z)) {
        return;
    }
    m = new_marker(mark, x, y, z);
    if (m == NULL) {
        return;
    }
    if (cheat) {
        m->flags |= PORT_AUTOMAP_MARK_CHEAT;
    }
    name_id = (s16)(def->definition_flags & 0xFFFF);
    message_text(name_id, name, sizeof(name));
    switch (mark) {
    case PORT_AUTOMAP_MARK_DOOR:
    case PORT_AUTOMAP_MARK_LOCKED_DOOR: {
        if (leaf != NULL) {
            m->x = leaf[0];
            m->z = leaf[1];
            m->angle = atan2f(leaf[3] - leaf[1], leaf[2] - leaf[0]);
            m->length = sqrtf((leaf[2] - leaf[0]) * (leaf[2] - leaf[0]) + (leaf[3] - leaf[1]) * (leaf[3] - leaf[1]));
        }
        add_line(m->text, sizeof(m->text), name[0] ? name : mark == PORT_AUTOMAP_MARK_LOCKED_DOOR ? "Locked door" : "Door");
        if (done) {
            m->flags |= PORT_AUTOMAP_MARK_DONE;
            add_line(m->text, sizeof(m->text), "(open)");
        }
        if (mark == PORT_AUTOMAP_MARK_LOCKED_DOOR && port_automap.hints) {
            lock_line(m->text, sizeof(m->text), words->kind_param_0);
        }
        break;
    }
    case PORT_AUTOMAP_MARK_CHEST:
        add_line(m->text, sizeof(m->text), name[0] ? name : "Chest");
        if (done) {
            m->flags |= PORT_AUTOMAP_MARK_DONE;
            add_line(m->text, sizeof(m->text), "(opened)");
        }
        /* MapObject_UpdateClosedChest: its item when it names one, else its gold */
        put_out = src->kind_param_0 != 0 ? item_put_out(t, src->kind_param_0) : 0;
        contents(m, t, src, name, put_out != 0 ? src->kind_param_0 : 0, put_out, src->kind_param_0 != 0 ? 0 : words->kind_param_2,
                 emptied, 0);
        if (!done && words->kind_param_1 != 0 && port_automap.hints) {
            lock_line(m->text, sizeof(m->text), words->kind_param_1);
        }
        break;
    case PORT_AUTOMAP_MARK_REMAINS:
    case PORT_AUTOMAP_MARK_CONTAINER:
        add_line(m->text, sizeof(m->text), name[0] ? name : mark == PORT_AUTOMAP_MARK_CONTAINER ? "Container" : "Remains");
        /* The item it holds comes out when the player uses it (bones and graves, MapObject_UpdateItemHolder,
           when no use script takes the press; a container only shows its name) or breaks it (definition flag
           0x800, MapObject_UpdateBreakingItemHolder: the containers; no bones or grave can be broken). */
        put_out = (mark == PORT_AUTOMAP_MARK_REMAINS && src->use_script == 0) || (*(const u32 *)def & 0x800)
                      ? item_put_out(t, src->kind_param_0)
                      : 0;
        if (!contents(m, t, src, name, put_out != 0 ? src->kind_param_0 : 0, put_out, 0, emptied, 1)) {
            m->flags |= PORT_AUTOMAP_MARK_DONE;
        }
        break;
    case PORT_AUTOMAP_MARK_SAVE:
        add_line(m->text, sizeof(m->text), "Save point");
        message_text(src->kind_param_0, name, sizeof(name));
        add_line(m->text, sizeof(m->text), name);
        break;
    case PORT_AUTOMAP_MARK_SWITCH:
        add_line(m->text, sizeof(m->text), name[0] ? name : "Switch");
        if (done) {
            m->flags |= PORT_AUTOMAP_MARK_DONE;
            add_line(m->text, sizeof(m->text), "(on)");
        }
        break;
    case PORT_AUTOMAP_MARK_TRAP:
        add_line(m->text, sizeof(m->text), kind == 16 ? "Trap: it shoots" : "Trap: the floor opens");
        break;
    case PORT_AUTOMAP_MARK_SECRET:
        add_line(m->text, sizeof(m->text), "Hidden: a wall or object that appears or fades away");
        break;
    case PORT_AUTOMAP_MARK_PASSAGE:
        add_line(m->text, sizeof(m->text), name[0] ? name : "Symbol of passage");
        break;
    case PORT_AUTOMAP_MARK_PEDESTAL:
        add_line(m->text, sizeof(m->text), name[0] ? name : kind == 19 ? "Marker" : "Pedestal");
        if (kind == 13 && emptied) {
            m->flags |= PORT_AUTOMAP_MARK_DONE;
        }
        break;
    case PORT_AUTOMAP_MARK_EXAMINE:
        /* what examining it shows (MapObject_StartExaminable): a sign picture, or a message */
        if (words->kind_param_1 != 0) {
            add_line(m->text, sizeof(m->text), "Sign");
        } else {
            char said[160];

            message_text(src->kind_param_0 != 0 ? src->kind_param_0 : name_id, said, sizeof(said));
            add_line(m->text, sizeof(m->text), said[0] ? said : "Something to look at");
        }
        break;
    case PORT_AUTOMAP_MARK_LADDER: add_line(m->text, sizeof(m->text), name[0] ? name : "Ladder"); break;
    case PORT_AUTOMAP_MARK_LIFT: add_line(m->text, sizeof(m->text), name[0] ? name : "Moving platform"); break;
    default: add_line(m->text, sizeof(m->text), name[0] ? name : "Object"); break;
    }
    hint = Automap_ObjectHint(plan.map, i);
    if (hint != NULL) {
        m->flags |= PORT_AUTOMAP_MARK_HINT;
        if (port_automap.hints) {
            add_line(m->text, sizeof(m->text), hint);
        }
    }
    if (port_automap.hints) {
        char line[32];

        snprintf(line, sizeof(line), "(object #%d)", i);
        add_line(m->text, sizeof(m->text), line);
    }
}

static int is_door(int kind) {
    return kind == 1 || kind == 2 || kind == 6 || kind == 17;
}

static void object_markers(int slot) {
    MapTables t = map_tables(slot);
    int count = map_object_counts[slot], i;

    for (i = 0; i < count; i++) {
        MapObject *o = &map_slot_objects[slot][i];
        float leaf[4];
        int has_leaf;

        if (o->definition == NULL || o->source == NULL || !(o->state_flags & MAP_OBJECT_IN_USE)) {
            continue;
        }
        has_leaf = is_door(o->definition->kind) && door_leaf(o->model, &leaf[0], &leaf[1], &leaf[2], &leaf[3]);
        object_marker(&t, i, o->definition, o->source, (u32)o->state_flags,
                      o->model != NULL ? o->model->position.x : o->source->position.x,
                      o->model != NULL ? o->model->position.y : o->source->position.y,
                      o->model != NULL ? o->model->position.z : o->source->position.z, has_leaf ? leaf : NULL);
    }
}

/* Whether the game would put spawn `i` of the map in slot `slot` in the world when the player comes near:
   EntityAi_ShouldRespawn's test without the distance and the chance roll. Entity_UpdateAll brings an
   entity on the active list that is not live (Entity_ResetToSpawn clears the live bit when it is given
   a model) back only through it. The flags it reads are the spawn's own (Entity_InitFromMapSpawns: spawn
   flags bits 59-60 at 2, set up by a script only) and the ones the map's scripts set as the story moves on
   (command 253, ScriptCmd_SetEntityFlags: bit 7, may come into the world), with the deaths counted
   against the spawn's limit (Game_GetSpawnDeathCount). */
static int spawn_flags_can_appear(u32 flags, const EntitySpawnInfo *si, int map, int i) {
    u64 spawn_flags = si->spawn_flags;
    int limit = (s8)(u8)(spawn_flags >> 40);

    if (!(flags & ENTITY_FLAG_RESPAWNABLE)) {
        return 0;
    }
    if ((flags & ENTITY_FLAG_NO_RESPAWN) && (spawn_flags & (1ULL << 61))) {
        return 0;
    }
    if (limit != 0 && Game_GetSpawnDeathCount(map, i) >= limit) {
        return 0;
    }
    return 1;
}

static int spawn_can_appear(int slot, int i, const Entity *e) {
    return spawn_flags_can_appear((u32)e->flags, e->spawn_info, map_slots[slot].tag, i);
}

/* in the world now: live, not dying, and on the active list (so given a model and put at its place;
   an entity off it keeps the bits it had, and the model's position means nothing) */
static int entity_live(const Entity *e) {
    return (e->flags & ENTITY_FLAG_LIVE) && !(e->flags & ENTITY_FLAG_DYING_MASK) && (e->flags & ENTITY_FLAG_ACTIVE_LIST) &&
           e->model != NULL;
}

/* a body lying as set dressing, not a creature to fight: a spawn whose type's set-up gives it only a pose to
   hold (the Expedition Soldiers of variant 1 and 2 lying dead on map 012, ExpeditionSoldier_ThinkHoldAnim18Or19:
   no on_hit, no death handler, and every frame flagged out of targeting and collision). The statues and
   guardians that hold a pose until woken keep their on_hit handler, and stay enemies. */
static int entity_prop(const Entity *e) {
    return (e->flags & ENTITY_FLAG_NOT_TARGETABLE) && e->ai_state.on_hit == NULL && e->ai_state.death == NULL;
}

/* a person's marker (the people of map slots and of browsed maps): his name, and when he is not here now (the
   secrets cheat), drawn faint */
static PortAutomapMarker *person_marker(int i, const EntitySpawnInfo *si, const EntityParams *p, float x, float y, float z,
                                        int later) {
    PortAutomapMarker *m = new_marker(PORT_AUTOMAP_MARK_PERSON, x, y, z);
    char name[96], line[128];

    if (m == NULL) {
        return NULL;
    }
    if (p->entity_type == 81) {
        snprintf(name, sizeof(name), "Dog");
    } else {
        person_name(si->type_id, p->model_number, name, sizeof(name));
    }
    add_line(m->text, sizeof(m->text), name);
    if (later) {
        m->flags |= PORT_AUTOMAP_MARK_CHEAT | PORT_AUTOMAP_MARK_ABSENT;
        add_line(m->text, sizeof(m->text), "(not here now: at another time in the story)");
    }
    if (port_automap.hints) {
        snprintf(line, sizeof(line), "(spawn #%d, chr %d)", i, si->type_id);
        add_line(m->text, sizeof(m->text), line);
    }
    return m;
}

/* a hand-written hint of a marker (the hints cheat shows it) */
static void hint_line(PortAutomapMarker *m, const char *hint) {
    if (hint != NULL) {
        m->flags |= PORT_AUTOMAP_MARK_HINT;
        if (port_automap.hints) {
            add_line(m->text, sizeof(m->text), hint);
        }
    }
}

static void entity_markers(int slot) {
    int count = map_spawn_counts[slot], i;

    for (i = 0; i < count; i++) {
        Entity *e = &entity_pool[slot][i];
        EntitySpawnInfo *si = e->spawn_info;
        EntityParams *p = e->params;
        int type, live, record, later = 0;
        float x, y, z;
        PortAutomapMarker *m;
        char name[96], line[128];
        const char *hint;

        if (si == NULL || p == NULL) {
            continue;
        }
        type = p->entity_type;
        record = si->type_id;
        live = entity_live(e);
        x = live ? e->model->position[0] : si->spawn_x;
        y = live ? e->model->position[1] : si->spawn_y;
        z = live ? e->model->position[2] : si->spawn_z;
        hint = Automap_SpawnHint(plan.map, i, record);
        if ((type == 15 || type == 81) && si->talk_script_id != 0) {
            /* a person not in the world now is shown where he will stand only if the game would bring him
               when the player comes near (he is out of reach, or not brought in yet); one the story has
               not brought here (or has taken away), or who has died, is not, except as a faint
               "not here now" with the secrets cheat. Not met yet: some are put in place by a script */
            if (!live && !spawn_can_appear(slot, i, e)) {
                if (!port_automap.secrets) {
                    continue;
                }
                later = 1;
            }
            if ((!live && x == 0.0f && y == 0.0f && z == 0.0f) || (!later && !seen_near(x, y, z))) {
                continue;
            }
            m = person_marker(i, si, p, x, y, z, later);
            if (m == NULL) {
                return;
            }
        } else {
            const char *creature;

            if (!live || e->hit_points == 0 || type == 15 || type == 81 || entity_prop(e)) {
                continue;
            }
            if (!port_automap.enemies && !(hint != NULL && port_automap.hints && seen_near(x, y, z))) {
                continue;
            }
            m = new_marker(PORT_AUTOMAP_MARK_ENEMY, x, y, z);
            if (m == NULL) {
                return;
            }
            m->flags |= PORT_AUTOMAP_MARK_CHEAT;
            creature = Automap_CreatureName(record);
            if (creature != NULL) {
                add_line(m->text, sizeof(m->text), creature);
            } else {
                snprintf(line, sizeof(line), "Creature (#%d)", record);
                add_line(m->text, sizeof(m->text), line);
            }
            snprintf(line, sizeof(line), "HP %d / %d", e->hit_points, p->hit_points);
            add_line(m->text, sizeof(m->text), line);
            if (port_automap.items && p->drop_item != 0xFFFF && p->drop_item != 0 && p->drop_chance > 0) {
                item_name(p->drop_item, name, sizeof(name));
                snprintf(line, sizeof(line), "May drop: %s (%d%%)", name, p->drop_chance);
                add_line(m->text, sizeof(m->text), line);
            }
        }
        hint_line(m, hint);
    }
}

/* ---- The map's script: the items it hands over ------------------------------------------------------
   Some of a map's fixed items (MapItemDef) are not there to be found: they lie out of sight (under the
   floor) until a script gives them to the player. Script command 101 (ScriptCmd_StartItemPickup,
   MapItem_StartPickupById) starts the pickup of a fixed item by its id (MapItemDef.slot_id) as if the
   player had taken it, flying it from where it lies to his eyes. Who hands it over: the person whose talk
   script (EntitySpawnInfo.talk_script_id) or whose script run when an item is used on him (item_script_id)
   reaches the command, itself or through command 2 (ScriptCmd_RunScript). The map's script (MapSlot.script)
   is read once as the map is built, its words stepped as Script_SkipWord steps them. */
#define MAX_SCRIPT_RUNS 512
#define MAX_SCRIPT_GIFTS 64

typedef struct {
    u16 label, target; /* target: a script run (command 2), or an item id given (command 101) */
} ScriptLink;

static ScriptLink script_runs[MAX_SCRIPT_RUNS];
static ScriptLink script_gifts[MAX_SCRIPT_GIFTS];
static ScriptLink script_messages[MAX_SCRIPT_RUNS]; /* target: the first message a script puts up (command 233) */
static int script_run_count, script_gift_count, script_message_count;

/* the script from `cursor` (to `end`, NULL: no end but the bound) */
static void scan_script(const u32 *cursor, const u32 *end) {
    int label = -1, words;

    script_run_count = script_gift_count = script_message_count = 0;
    if (cursor == NULL) {
        return;
    }
    /* a bound on the walk, in case the words are not what they should be */
    for (words = 0; words < 0x40000 && (end == NULL || cursor + 3 <= end); words++) {
        u32 word = *cursor;
        int operand = word & 0xFFFF;

        cursor++;
        switch (word >> 16) {
        case SCRIPT_WORD_LABEL:
            if (operand == SCRIPT_LABEL_END) {
                return;
            }
            label = operand;
            break;
        case SCRIPT_WORD_END:
        case SCRIPT_WORD_BEGIN_NO_WAIT:
        case SCRIPT_WORD_END_NO_WAIT:
        case SCRIPT_WORD_JUMP:
            break;
        case SCRIPT_WORD_IF: cursor += (operand >> 8) * 5 + 1; break;
        case SCRIPT_WORD_SET: cursor += operand; break;
        case SCRIPT_WORD_HALT: cursor += operand + 1; break;
        default:
            /* a command: its arguments are two words each, the first a constant when its low byte is 0 */
            if (label >= 0 && operand >= 1 && (cursor[0] & 0xFF) == 0) {
                if ((word >> 16) == 2 && script_run_count < MAX_SCRIPT_RUNS) {
                    script_runs[script_run_count].label = (u16)label;
                    script_runs[script_run_count++].target = (u16)cursor[1];
                } else if ((word >> 16) == 101 && script_gift_count < MAX_SCRIPT_GIFTS) {
                    script_gifts[script_gift_count].label = (u16)label;
                    script_gifts[script_gift_count++].target = (u16)cursor[1];
                } else if ((word >> 16) == 233 && script_message_count < MAX_SCRIPT_RUNS &&
                           (script_message_count == 0 || script_messages[script_message_count - 1].label != label)) {
                    script_messages[script_message_count].label = (u16)label;
                    script_messages[script_message_count++].target = (u16)cursor[1];
                }
            }
            cursor += operand * 2;
            break;
        }
    }
}

/* how far the point is below the nearest floor of the plan in its cell or the cells beside it (y grows
   downward: more than 0 is under it; 0x7FFF: no floor) */
static int floor_gap(float x, float y, float z) {
    int i, j, di, dj, best = 0x7FFF;

    if (!cell_of(x, z, &i, &j)) {
        return best;
    }
    for (dj = -1; dj <= 1; dj++) {
        for (di = -1; di <= 1; di++) {
            const PortAutomapLayer *l;
            int n;

            if (i + di < 0 || j + dj < 0 || i + di >= plan.width || j + dj >= plan.height) {
                continue;
            }
            l = cell_layers(i + di, j + dj);
            for (n = 0; n < LAYERS && l[n].y != NO_Y; n++) {
                int d = (int)y - l[n].y;

                if (l[n].kind != PORT_AUTOMAP_DEATH && abs(d) < abs(best)) {
                    best = d;
                }
            }
        }
    }
    return best;
}

/* whether the script `label` gives the item with this id, itself or through the scripts it runs */
static int script_gives(int label, int id, int depth) {
    int k;

    if (label == 0 || depth > 6) {
        return 0;
    }
    for (k = 0; k < script_gift_count; k++) {
        if (script_gifts[k].label == label && script_gifts[k].target == id) {
            return 1;
        }
    }
    for (k = 0; k < script_run_count; k++) {
        if (script_runs[k].label == label && script_runs[k].target != label && script_gives(script_runs[k].target, id, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

/* the fixed item the script `label` hands over (command 101), itself or through the scripts it runs: its id
   (MapItemDef.slot_id), or 0 */
static int script_gift(int label, int depth) {
    int k, id;

    if (label == 0 || depth > 6) {
        return 0;
    }
    for (k = 0; k < script_gift_count; k++) {
        if (script_gifts[k].label == label) {
            return script_gifts[k].target;
        }
    }
    for (k = 0; k < script_run_count; k++) {
        if (script_runs[k].label == label && script_runs[k].target != label &&
            (id = script_gift(script_runs[k].target, depth + 1)) != 0) {
            return id;
        }
    }
    return 0;
}

/* the first message the script `label` itself puts up (command 233, ScriptCmd_ShowNotice), or 0 */
static int script_message(int label) {
    int k;

    for (k = 0; label != 0 && k < script_message_count; k++) {
        if (script_messages[k].label == label) {
            return script_messages[k].target;
        }
    }
    return 0;
}

static int script_gives_anywhere(int id) {
    int k;

    for (k = 0; k < script_gift_count; k++) {
        if (script_gifts[k].target == id) {
            return 1;
        }
    }
    return 0;
}

/* a fixed item that is not there to be found but handed over: a script gives it (command 101), not the one
   run when the player takes it where it lies (MapItemDef.pickup_script: crystals on altars, Lord Myu's
   armour), and it lies out of sight, under the floor (more than 60 below the nearest floor of the plan, or
   with none near: the smith's work lies 10,000 below the world) */
static int item_handed_over_at(const MapItemDef *def, float x, float y, float z) {
    int id;

    if (def == NULL) {
        return 0;
    }
    id = def->slot_id;
    return id != 0 && script_gives_anywhere(id) && !script_gives(def->pickup_script, id, 0) && floor_gap(x, y, z) > 60;
}

static int item_handed_over(const ItemNode *n) {
    return n->model != NULL && item_handed_over_at(n->def, n->model->position.x, n->model->position.y, n->model->position.z);
}

/* the person whose talk (or the item used on him) gives the fixed item with this id: his spawn, or -1 */
static int item_giver(const MapTables *t, int id, int *by_item) {
    int i;

    for (i = 0; i < t->spawn_count; i++) {
        if (script_gives(t->spawns[i].talk_script_id, id, 0)) {
            *by_item = 0;
            return i;
        }
    }
    for (i = 0; i < t->spawn_count; i++) {
        if (script_gives(t->spawns[i].item_script_id, id, 0)) {
            *by_item = 1;
            return i;
        }
    }
    return -1;
}

/* an item's marker: `def` its fixed item (NULL: one dropped), at x, y, z; `note` a line more (or NULL) */
static void item_marker(const MapTables *t, int item, const MapItemDef *def, float x, float y, float z, const char *note) {
    PortAutomapMarker *m = new_marker(PORT_AUTOMAP_MARK_ITEM, x, y, z);
    char name[64];

    if (m == NULL) {
        return;
    }
    m->flags |= PORT_AUTOMAP_MARK_CHEAT;
    item_name(item, name, sizeof(name));
    add_line(m->text, sizeof(m->text), name);
    if (item_handed_over_at(def, x, y, z)) {
        /* not one to find: out of sight until the story hands it over */
        int by_item = 0, giver = item_giver(t, def->slot_id, &by_item);
        char line[160];

        m->flags |= PORT_AUTOMAP_MARK_GIVEN;
        if (giver >= 0) {
            const EntitySpawnInfo *si = &t->spawns[giver];
            const EntityParams *p = spawn_params(si);

            if (p->entity_type == 81) {
                snprintf(name, sizeof(name), "Dog");
            } else {
                person_name(si->type_id, p->model_number, name, sizeof(name));
            }
            snprintf(line, sizeof(line), by_item ? "(not on the ground: given when an item is used on %s)"
                                                 : "(not on the ground: given by %s)",
                     name);
            add_line(m->text, sizeof(m->text), line);
        } else {
            add_line(m->text, sizeof(m->text), "(not on the ground: given by an event)");
        }
    } else {
        add_line(m->text, sizeof(m->text), def != NULL ? "(lies here)" : "(dropped)");
    }
    add_line(m->text, sizeof(m->text), note);
}

/* the items lying in the map in `slot` (lying_items), not inside a container, moved by minus `offset` */
static void item_markers(int slot, const float offset[3]) {
    MapTables t = map_tables(slot);
    ItemNode *n;

    if (!port_automap.items) {
        return;
    }
    for (n = lying_items; n != NULL; n = n->next) {
        if ((int)(n->flags & 1) != slot || n->model == NULL || n->map_object != NULL) {
            continue;
        }
        item_marker(&t, n->item, n->def, n->model->position.x - offset[0], n->model->position.y - offset[1],
                    n->model->position.z - offset[2], NULL);
    }
}

/* The ways into the neighbouring maps. KF4's maps join without a load screen: a record of the map
   (MapRecordInfo.neighbour_origin_index) names one of the map's origins (MapSlot.origins, a MapLoadRequest:
   the neighbour's number and where it lies), and while the player stands on it the game loads that neighbour
   into its other slot, moved by minus the origin's position (MapLoad_Update, MapLoad_SetUpMap); a record with
   needs_neighbour_map waits for that load to end. Those records are where the loading starts (often whole
   rooms), not the way across. The player crosses when the floor under him is the other map's
   (Player_FindFloor: Map_MakeCurrent of the slot whose floor he stands on). So a way is where this map's floor
   meets the neighbour's floor, placed as the game places it: read from the neighbour's file on the disc (its
   records' floor, hazard and water strips, placed by each record's matrix as Map_PlaceRecordModels builds
   it), the cells of this map's floor with the neighbour's floor in them or beside them within a step (150).
   Each connected stretch of those cells is one way, marked over that stretch only. Worked out as a map is
   built. (Doors and wands that change the map by a script, MapChange_StartToMap, are not marked.) */
#define MAX_EXITS 64
#define EXIT_STEP 150

typedef struct {
    float x0, z0, x1, z1, y;
    int map;
    float to[3]; /* add to a point of this map for the same point in the neighbour's own frame (the origin) */
} Exit;

static Exit exits[MAX_EXITS];
static int exit_count;

/* the neighbour's floor heights per cell (up to LAYERS), for one neighbour at a time, and the cells of the
   records that load it (MapRecordInfo.neighbour_origin_index) */
static int16_t *neighbour_y;
static uint8_t *loads_neighbour;

static void neighbour_cell(int i, int j, float yf, void *context) {
    int16_t *l = &neighbour_y[((size_t)j * plan.width + i) * LAYERS];
    int y = (int)yf, n;

    (void)context;
    if (y <= -32000 || y >= 32000) {
        return;
    }
    for (n = 0; n < LAYERS && l[n] != NO_Y; n++) {
        if (abs(l[n] - y) < MERGE) {
            return;
        }
    }
    if (n < LAYERS) {
        l[n] = (int16_t)y;
    }
}

/* a triangle of the neighbour's floor, water or hazards */
static void neighbour_triangle(const float a[3], const float b[3], const float c[3], int effect, void *context) {
    (void)effect;
    raster(a, b, c, neighbour_cell, context);
}

/* the layer of cell c that has the neighbour's floor in its cell or one beside it, within a step: its
   height, or NO_Y */
static int meets_neighbour(int c) {
    const PortAutomapLayer *l = &layers[(size_t)c * LAYERS];
    int i = c % plan.width, j = c / plan.width, n, d, k;
    static const int di[5] = {0, 1, -1, 0, 0}, dj[5] = {0, 0, 0, 1, -1};

    for (n = 0; n < LAYERS && l[n].y != NO_Y; n++) {
        if (l[n].kind == PORT_AUTOMAP_DEATH) {
            continue;
        }
        for (d = 0; d < 5; d++) {
            const int16_t *o;

            if (i + di[d] < 0 || j + dj[d] < 0 || i + di[d] >= plan.width || j + dj[d] >= plan.height) {
                continue;
            }
            o = &neighbour_y[((size_t)(j + dj[d]) * plan.width + i + di[d]) * LAYERS];
            for (k = 0; k < LAYERS && o[k] != NO_Y; k++) {
                if (abs(o[k] - l[n].y) <= EXIT_STEP) {
                    return l[n].y;
                }
            }
        }
    }
    /* a gap in this map's floor (no floor, or only the void) with the neighbour's floor in it, at or below the
       floor beside it, where a record that loads the neighbour lies: the player drops in */
    if ((n == 0 || (n == 1 && l[0].kind == PORT_AUTOMAP_DEATH)) && loads_neighbour[c]) {
        const int16_t *o = &neighbour_y[(size_t)c * LAYERS];

        for (d = 1; d < 5 && o[0] != NO_Y; d++) {
            const PortAutomapLayer *b;

            if (i + di[d] < 0 || j + dj[d] < 0 || i + di[d] >= plan.width || j + dj[d] >= plan.height) {
                continue;
            }
            b = &layers[((size_t)(j + dj[d]) * plan.width + i + di[d]) * LAYERS];
            for (n = 0; n < LAYERS && b[n].y != NO_Y; n++) {
                for (k = 0; b[n].kind != PORT_AUTOMAP_DEATH && k < LAYERS && o[k] != NO_Y; k++) {
                    if (o[k] > b[n].y - EXIT_STEP) {
                        return b[n].y;
                    }
                }
            }
        }
    }
    return NO_Y;
}

/* the ways of the plan in hand: `records` (count of them) name the origins (32 MapLoadRequest of 16 bytes) */
static void find_exits(const char *records, int count, const char *origins) {
    int i, k, o;
    size_t cells = (size_t)plan.width * plan.height;
    uint8_t used[32] = {0}, *way;
    int *stack;

    exit_count = 0;
    for (i = 0; i < count; i++) {
        const MapRecordInfo *info = (const MapRecordInfo *)(records + i * 0x80);

        if (info->neighbour_origin_index < 32) {
            used[info->neighbour_origin_index] = 1;
        }
    }
    neighbour_y = malloc(cells * LAYERS * sizeof(*neighbour_y));
    loads_neighbour = malloc(cells);
    way = malloc(cells);
    stack = malloc(cells * sizeof(*stack));
    for (o = 0; o < 32 && neighbour_y != NULL && loads_neighbour != NULL && way != NULL && stack != NULL; o++) {
        const MapLoadRequest *request = (const MapLoadRequest *)(origins + 16 * o);
        int map = request->map & 0xFF, again = 0;
        float offset[3];
        MapFile file;

        if (!used[o] || map == plan.map) {
            continue;
        }
        for (k = 0; k < o; k++) { /* the same neighbour in the same place through another origin */
            const MapLoadRequest *before = (const MapLoadRequest *)(origins + 16 * k);

            again |= used[k] && (before->map & 0xFF) == map && before->x == request->x && before->y == request->y &&
                     before->z == request->z;
        }
        if (again || !map_file_read(map, &file)) {
            continue;
        }
        for (k = 0; k < (int)(cells * LAYERS); k++) {
            neighbour_y[k] = NO_Y;
        }
        offset[0] = -request->x;
        offset[1] = -request->y;
        offset[2] = -request->z;
        file_triangles(&file, offset, 1 << 1 | 1 << 4 | 1 << 5, neighbour_triangle, NULL);
        map_file_free(&file);
        memset(loads_neighbour, 0, cells);
        for (i = 0; i < count; i++) {
            const MapRecordBounds *r = (const MapRecordBounds *)(records + i * 0x80);
            const MapRecordInfo *info = (const MapRecordInfo *)r;
            const MapLoadRequest *to;
            int i0, i1, j0, j1, ci, cj;

            if (info->neighbour_origin_index >= 32) {
                continue;
            }
            to = (const MapLoadRequest *)(origins + 16 * info->neighbour_origin_index);
            if ((to->map & 0xFF) != map) {
                continue;
            }
            i0 = (int)floorf((r->lowest[0] - plan.x0) / CELL);
            i1 = (int)ceilf((r->highest[0] - plan.x0) / CELL);
            j0 = (int)floorf((r->lowest[2] - plan.z0) / CELL);
            j1 = (int)ceilf((r->highest[2] - plan.z0) / CELL);
            for (cj = j0 < 0 ? 0 : j0; cj < j1 && cj < plan.height; cj++) {
                for (ci = i0 < 0 ? 0 : i0; ci < i1 && ci < plan.width; ci++) {
                    loads_neighbour[(size_t)cj * plan.width + ci] = 1;
                }
            }
        }
        for (k = 0; k < (int)cells; k++) {
            way[k] = meets_neighbour(k) != NO_Y;
        }
        /* each connected stretch of them, one way */
        for (k = 0; k < (int)cells && exit_count < MAX_EXITS; k++) {
            int top = 0, n = 0;
            float sum_y = 0.0f;
            Exit *e;

            if (!way[k]) {
                continue;
            }
            e = &exits[exit_count++];
            e->x0 = e->z0 = 1e30f;
            e->x1 = e->z1 = -1e30f;
            e->map = map;
            e->to[0] = request->x;
            e->to[1] = request->y;
            e->to[2] = request->z;
            way[k] = 0;
            stack[top++] = k;
            while (top > 0) {
                int c = stack[--top], ci = c % plan.width, cj = c / plan.width, di, dj;

                sum_y += meets_neighbour(c);
                n++;
                e->x0 = fminf(e->x0, plan.x0 + ci * CELL);
                e->z0 = fminf(e->z0, plan.z0 + cj * CELL);
                e->x1 = fmaxf(e->x1, plan.x0 + (ci + 1) * CELL);
                e->z1 = fmaxf(e->z1, plan.z0 + (cj + 1) * CELL);
                for (dj = -1; dj <= 1; dj++) {
                    for (di = -1; di <= 1; di++) {
                        int ni = ci + di, nj = cj + dj;

                        if (ni >= 0 && nj >= 0 && ni < plan.width && nj < plan.height && way[nj * plan.width + ni]) {
                            way[nj * plan.width + ni] = 0;
                            stack[top++] = nj * plan.width + ni;
                        }
                    }
                }
            }
            e->y = sum_y / n;
        }
    }
    free(neighbour_y);
    neighbour_y = NULL;
    free(loads_neighbour);
    loads_neighbour = NULL;
    free(way);
    free(stack);
}

static void exit_markers(void) {
    int k;

    for (k = 0; k < exit_count; k++) {
        const Exit *e = &exits[k];
        PortAutomapMarker *m;
        char line[128];
        const char *name;

        if (e->map < 0 || e->map == plan.map) {
            continue;
        }
        if (!seen_near((e->x0 + e->x1) * 0.5f, e->y, (e->z0 + e->z1) * 0.5f) &&
            !seen_near(e->x0, e->y, e->z0) && !seen_near(e->x1, e->y, e->z1)) {
            continue;
        }
        m = new_marker(PORT_AUTOMAP_MARK_EXIT, e->x0, e->y, e->z0);
        if (m == NULL) {
            return;
        }
        m->length = e->x1 - e->x0;
        m->depth = e->z1 - e->z0;
        m->map = e->map;
        m->to[0] = (e->x0 + e->x1) * 0.5f + e->to[0];
        m->to[1] = e->y + e->to[1];
        m->to[2] = (e->z0 + e->z1) * 0.5f + e->to[2];
        name = port_teleport_map_name(e->map);
        snprintf(line, sizeof(line), "To %s", name[0] ? name : "another map");
        add_line(m->text, sizeof(m->text), line);
        snprintf(line, sizeof(line), "(map %03d)", e->map);
        add_line(m->text, sizeof(m->text), line);
        /* the browser shows a map the player has been in (its explored part is kept), or any with the reveal cheat */
        if (port_automap.reveal || e->map == player.map || map_explored(e->map)) {
            m->flags |= PORT_AUTOMAP_MARK_BROWSE;
            add_line(m->text, sizeof(m->text), "Click: show this map");
        } else {
            add_line(m->text, sizeof(m->text), "(not visited yet)");
        }
    }
}

static void build_markers(int slot) {
    marker_count = 0;
    static const float none[3] = {0.0f, 0.0f, 0.0f};

    exit_markers();
    object_markers(slot);
    item_markers(slot, none);
    entity_markers(slot);
}

/* the words of the script `label` of the current map's script, for the dump: each command with its first
   argument (a constant) */
static void dump_script(FILE *f, int label) {
    const u32 *cursor = (const u32 *)map_slots[current_map_slot & 1].script;
    int in = 0, words;

    if (cursor == NULL || label == 0) {
        return;
    }
    fprintf(f, "  script %d:", label);
    for (words = 0; words < 0x40000; words++) {
        u32 word = *cursor;
        int operand = word & 0xFFFF;

        cursor++;
        switch (word >> 16) {
        case SCRIPT_WORD_LABEL:
            if (in || operand == SCRIPT_LABEL_END) {
                fprintf(f, "\n");
                return;
            }
            in = operand == label;
            break;
        case SCRIPT_WORD_END: if (in) fprintf(f, " end"); break;
        case SCRIPT_WORD_BEGIN_NO_WAIT:
        case SCRIPT_WORD_END_NO_WAIT:
        case SCRIPT_WORD_JUMP: break;
        case SCRIPT_WORD_IF:
            if (in) {
                int k;

                fprintf(f, " if(");
                for (k = 0; k < (operand >> 8) * 5 + 1; k++) {
                    fprintf(f, "%s%x", k ? " " : "", cursor[k]);
                }
                fprintf(f, ")");
            }
            cursor += (operand >> 8) * 5 + 1;
            break;
        case SCRIPT_WORD_SET:
            if (in) {
                int k;

                fprintf(f, " set(");
                for (k = 0; k < operand; k++) {
                    fprintf(f, "%s%x", k ? " " : "", cursor[k]);
                }
                fprintf(f, ")");
            }
            cursor += operand;
            break;
        case SCRIPT_WORD_HALT: cursor += operand + 1; break;
        default:
            if (in) {
                int k;

                fprintf(f, " cmd%d(", word >> 16);
                for (k = 0; k < operand; k++) {
                    fprintf(f, "%s%x:%d", k ? " " : "", cursor[2 * k] & 0xFF, (int)cursor[2 * k + 1]);
                }
                fprintf(f, ")");
            }
            cursor += operand * 2;
            break;
        }
    }
    fprintf(f, "\n");
}

/* KF4_AUTOMAP_DUMP=DIR: a while after each map is built, its markers' labels into DIR/automap_NNN.txt (for
   checking the names and the hints without a window) */
static void dump_markers(int slot) {
    const char *dir = getenv("KF4_AUTOMAP_DUMP");
    MapTables t = map_tables(slot);
    char path[1024];
    FILE *f;
    int i;

    if (dir == NULL) {
        return;
    }
    snprintf(path, sizeof(path), "%s/automap_%03d.txt", dir, plan.map);
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "map %d: %dx%d cells from %d,%d, layout %08x\n", plan.map, plan.width, plan.height, (int)plan.x0, (int)plan.z0,
            layout_hash);
    for (i = 0; i < marker_count; i++) {
        char text[256], *c;

        snprintf(text, sizeof(text), "%s", markers[i].text);
        for (c = text; *c; c++) {
            if (*c == '\n') {
                *c = '|';
            }
        }
        fprintf(f, "%d kind %d flags %d at %d %d %d: %s\n", i, markers[i].kind, markers[i].flags, (int)markers[i].x,
                (int)markers[i].y, (int)markers[i].z, text);
    }
    /* and every spawn, with what entity_markers decides from */
    for (i = 0; i < map_spawn_counts[slot]; i++) {
        const Entity *e = &entity_pool[slot][i];

        if (e->spawn_info == NULL || e->params == NULL) {
            continue;
        }
        fprintf(f, "spawn %d chr %d type %d talk %d flags %08x live %d prop %d hp %d variant %d can_appear %d deaths %d at %d %d %d\n", i,
                e->spawn_info->type_id, e->params->entity_type, e->spawn_info->talk_script_id, (u32)e->flags,
                entity_live(e), entity_prop(e), e->hit_points, e->spawn_info->variant, spawn_can_appear(slot, i, e),
                Game_GetSpawnDeathCount(map_slots[slot].tag, i),
                (int)e->spawn_info->spawn_x, (int)e->spawn_info->spawn_y, (int)e->spawn_info->spawn_z);
    }
    /* and every map object in use, with its scripts and parameters */
    for (i = 0; i < map_object_counts[slot]; i++) {
        const MapObject *o = &map_slot_objects[slot][i];
        const MapObjectUnk08Words *w;

        if (o->definition == NULL || o->source == NULL || !(o->state_flags & MAP_OBJECT_IN_USE)) {
            continue;
        }
        w = (const MapObjectUnk08Words *)o->source;
        fprintf(f, "object %d kind %d state %08x def %08x name %d use_script %d item_use_script %d break_script %d params %d %d %d %d put_out %d\n",
                i, o->definition->kind, (u32)o->state_flags, MAP_OBJECT_DEF_WORD(o), (s16)(o->definition->definition_flags & 0xFFFF),
                o->source->use_script, o->source->item_use_script, o->source->break_script, w->kind_param_0, w->kind_param_1,
                w->kind_param_2, w->kind_param_3, item_put_out(&t, w->kind_param_0));
        dump_script(f, o->source->use_script);
        dump_script(f, o->source->item_use_script);
    }
    for (i = 0; i < script_gift_count; i++) {
        fprintf(f, "script %d gives item id %d\n", script_gifts[i].label, script_gifts[i].target);
    }
    for (i = 0; i < script_run_count; i++) {
        fprintf(f, "script %d runs %d\n", script_runs[i].label, script_runs[i].target);
    }
    {
        ItemNode *n;

        for (n = lying_items; n != NULL; n = n->next) {
            if ((int)(n->flags & 1) != slot || n->model == NULL || n->def == NULL) {
                continue;
            }
            fprintf(f, "item %d gift %d (index %d id %d) chance %d group %d required %d pickup_script %d object %d floor_gap %d at %d %d %d def %d %d %d\n",
                    n->item, item_handed_over(n), n->index, n->def->slot_id, n->def->chance, n->def->group, n->def->required_flag,
                    n->def->pickup_script, n->map_object != 0,
                    floor_gap(n->model->position.x, n->model->position.y, n->model->position.z), (int)n->model->position.x, (int)n->model->position.y,
                    (int)n->model->position.z, (int)n->def->position.x, (int)n->def->position.y, (int)n->def->position.z);
        }
    }
    fclose(f);
}

/* ---- The map browser ---------------------------------------------------------------------------------
   On the full map, a way into another map can be clicked to show that map (docs/port/AUTOMAP.md, "Browsing
   other maps"), and its ways in turn, with Back (a history) and Home (the player's own map). A browsed map is
   built as the player's is, from its file on the disc instead of a map slot: the floor plan from its records'
   collision meshes (file_triangles), its ways from its origins, its fog of war from the explored state kept
   for it (read only), its markers from its objects, spawns and fixed items and from the save's tables: the
   objects' state as the map keeps it when the player leaves (map_entry_flags: Game_GetObjectActive,
   Game_GetObjectState, Game_GetObjectFlag, as MapObject_StartMap reads them back), the deaths of its spawns
   (Game_GetSpawnDeathCount), the items taken (map_item_pickup_counts) and dropped (dropped_item_records).
   The spawns' and objects' flags are not kept: they are made again as the map is loaded (from the spawn
   records, Entity_InitFromMapSpawns; MapSetup_RunForMap) and set by the map's start script as it becomes the
   current one, which game/automap_script.c runs on copies; so a person is shown when he would be there as the
   player walks in. A map still loaded in the game's other slot gives its live flags and items instead.

   Nothing of the game is written: the file is read into memory of its own (map_file_read), the tables are
   only read. It all runs on the game's thread while the full map holds the game (the overlay's draw), so no
   load of the game's can be under way at the same time.

   The browsed map is built in the globals of the map in hand (plan, layers, the ways, the script's links, the
   markers), swapped in for the time it takes (view_swap), so that every function above works on it. */
typedef struct {
    PortAutomapPlan plan;
    PortAutomapLayer *layers;
    int16_t *solid_top, *solid_bottom;
    uint8_t *seen;
    uint32_t layout_hash;
    Exit exits[MAX_EXITS];
    int exit_count;
    ScriptLink runs[MAX_SCRIPT_RUNS], gifts[MAX_SCRIPT_GIFTS], messages[MAX_SCRIPT_RUNS];
    int run_count, gift_count, message_count;
    PortAutomapMarker *markers;
    int marker_count;
} MapView;

static MapView browsed = {{-1}};
static int browse_cold;

#define SWAP(a, b)                                                                                                   \
    do {                                                                                                             \
        char swap_[sizeof(a)];                                                                                       \
        memcpy(swap_, &(a), sizeof(a));                                                                              \
        memcpy(&(a), &(b), sizeof(a));                                                                               \
        memcpy(&(b), swap_, sizeof(a));                                                                              \
    } while (0)

/* the browsed map in (or out of) the globals of the map in hand */
static void view_swap(MapView *v) {
    if (v->markers == NULL) {
        v->markers = browsed_markers;
    }
    SWAP(plan, v->plan);
    SWAP(layers, v->layers);
    SWAP(solid_top, v->solid_top);
    SWAP(solid_bottom, v->solid_bottom);
    SWAP(seen, v->seen);
    SWAP(layout_hash, v->layout_hash);
    SWAP(exits, v->exits);
    SWAP(exit_count, v->exit_count);
    SWAP(script_runs, v->runs);
    SWAP(script_gifts, v->gifts);
    SWAP(script_messages, v->messages);
    SWAP(script_run_count, v->run_count);
    SWAP(script_gift_count, v->gift_count);
    SWAP(script_message_count, v->message_count);
    SWAP(markers, v->markers);
    SWAP(marker_count, v->marker_count);
}

/* the game's slot that holds `map` now (loaded), or -1 */
static int loaded_slot(int map) {
    int k;

    for (k = 0; k < 2; k++) {
        if (map_slots[k].loaded && map_slots[k].tag == map && map_slots[k].header != NULL && map_slots[k].records != NULL) {
            return k;
        }
    }
    return -1;
}

/* What MapSetup_RunForMap does to the objects, spawns and fixed items of the maps it sets up (their records,
   fog and lights are not on the map), after MapObject_StartMap, Entity_InitFromMapSpawns and
   MapItem_RollFixedItems, on the copies. Its writes to the story's flags (map 100) are left out. */
static void setup_for_map(AutomapScriptWorld *w, MapFile *f) {
    extern u8 story_flags[0x40];
    extern u8 story_values[0x400];
    /* the names map/map_setup.c gives these places of the story's flags */
    const u8 *forge_door_open = story_flags + 0xC, *forge_stage = story_flags + 6;
    const u8 *craftsmans_room_door_unlocked = map_script_flags + 0x1A08;
    float map_record_height;
    int o, e;

#define OBJECT(id) ((o = w->object_by_id[(id)]) >= 0)
#define SPAWN(id) ((e = w->spawn_by_id[(id)]) >= 0)
#define RECORD(prop, hidden)                                                                                         \
    do {                                                                                                             \
        int r_ = w->record_by_prop[(prop)];                                                                          \
        if (r_ >= 0 && r_ < w->record_count) {                                                                       \
            w->record_hidden[r_] = (hidden);                                                                         \
        }                                                                                                            \
    } while (0)
#define PLACE(id)                                                                                                    \
    do {                                                                                                             \
        int i_ = w->item_by_id[(id)];                                                                                \
        if (i_ >= 0 && !w->item_taken[i_]) {                                                                         \
            w->item_placed[i_] = 1;                                                                                  \
        }                                                                                                            \
    } while (0)
    switch (w->map) {
    case 11: /* MapSetup_Armory */
        if (map_script_flags[0x2C5] != 0) {
            PLACE(0x46);
        }
        if (entity_group_state[0x5C6] == 1 && map_script_flags[0x2C5] == 0) {
            PLACE(0x46);
            if (entity_group_state[0x5C0] == 0 && SPAWN(0x17)) {
                w->entity_flags[e] |= ENTITY_FLAG_LIVE | ENTITY_FLAG_RESPAWNABLE;
            }
        } else {
            if (OBJECT(0xA)) {
                w->object_state[o] |= MAP_OBJECT_ON;
            }
            if (OBJECT(0xB)) {
                w->object_state[o] |= MAP_OBJECT_ON;
            }
        }
        break;
    case 20: /* MapSetup_EarthFolksVillage: the records of the forge's stage */
        RECORD(0x33, forge_stage[0] != 0);
        RECORD(0x6D, forge_stage[0] != 1);
        RECORD(0x6E, forge_stage[0] < 2);
        if (forge_stage[0] != 0) {
            RECORD(2, 1);
        }
        break;
    case 25: /* MapSetup_FireLordsDomain */
        RECORD(0x42, forge_stage[0] != 2);
        RECORD(0x43, forge_stage[0] != 2);
        RECORD(0x44, forge_stage[0] != 2);
        RECORD(0x3F, forge_stage[0] == 2);
        RECORD(0x41, forge_stage[0] == 2);
        break;
    case 29: /* MapSetup_UndergroundCanal: record 5 (the lift) at the height the story keeps for it */
        e = w->record_by_prop[5];
        if (e >= 0 && e < w->record_count && f->record_move != NULL) {
            memcpy(&map_record_height, story_values + 0xC, 4);
            f->record_move[e][1] = (map_script_flags[0x740] == 0 ? 590.0f : map_record_height) - 590.0f;
        }
        break;
    case 17: case 18: case 19: /* MapSetup_Forge017 to 019 */
        if (forge_door_open[0] != 0 && OBJECT(0x65)) {
            w->object_state[o] |= MAP_OBJECT_ON;
        }
        break;
    case 42: /* MapSetup_SanctuaryOfLight */
        if (map_script_flags[0xA85] != 0) {
            if (map_script_flags[0xA86] == 0) {
                PLACE(0xC9);
                if (OBJECT(0x67)) {
                    w->object_state[o] |= MAP_OBJECT_IN_USE;
                }
            }
        } else {
            if (OBJECT(0x66)) {
                w->object_state[o] &= ~MAP_OBJECT_IN_USE;
            }
            if (OBJECT(0x67)) {
                w->object_state[o] &= ~MAP_OBJECT_IN_USE;
            }
        }
        break;
    case 100: /* MapSetup_CentralTowerLevel3 */
        if (entity_group_state[0x340] == 0 &&
            (entity_group_state[0x346] == 1 || (entity_group_state[0x345] != 0 && entity_group_state[0x346] == 0))) {
            break;
        }
        if (SPAWN(1)) {
            w->entity_flags[e] &= ~(ENTITY_FLAG_LIVE | ENTITY_FLAG_RESPAWNABLE);
        }
        break;
    case 104: /* MapSetup_CraftsmansRoom */
        if (craftsmans_room_door_unlocked[0] == 0) {
            if (OBJECT(0x77)) {
                w->object_state[o] &= ~MAP_OBJECT_IN_USE;
            }
            if (OBJECT(0x76)) {
                w->object_state[o] &= ~MAP_OBJECT_IN_USE;
            }
        }
        break;
    case 107: /* MapSetup_CentralTowerLevel2 */
        if (map_script_flags[0x1C0A] != 0) {
            if (OBJECT(0x75)) {
                w->object_state[o] &= ~MAP_OBJECT_IN_USE;
            }
            if (OBJECT(0x76)) {
                w->object_state[o] |= MAP_OBJECT_IN_USE;
            }
        }
        break;
    }
#undef OBJECT
#undef SPAWN
#undef PLACE
#undef RECORD
}

/* a door's leaf from the file: its collision mesh's two farthest apart points (as door_leaf), placed by the
   object's matrix */
static int file_door_leaf(const MapFile *f, const MapObjectUnk08 *src, float leaf[4]) {
    const uint8_t *d = f->data;
    uint32_t at, points, point_count, strip_count, cursor, a, b;
    float m[16], px[64], pz[64], best = -1.0f;
    int k;

    if (src->collision_mesh_index == 0xFFFF || src->collision_mesh_index >= f->header->object_collision_count ||
        (at = file_mesh(f, (uint32_t)f->header->object_collision_offset, src->collision_mesh_index)) == 0) {
        return 0;
    }
    strip_count = file_u32(d + at);
    point_count = file_u32(d + at + 0x0C);
    cursor = at + 0x50;
    for (k = 0; k < (int)strip_count && cursor + 0x20 <= f->size; k++) {
        cursor += 0x20 + (uint32_t)(d[cursor] | d[cursor + 1] << 8) * 16;
    }
    points = cursor;
    point_count = point_count < 64 ? point_count : 64;
    if ((uint64_t)points + (uint64_t)point_count * 16 > f->size) {
        return 0;
    }
    file_matrix((const uint8_t *)src, 1, m);
    for (a = 0; a < point_count; a++) {
        const uint8_t *v = d + points + 16 * a;
        float x = file_f32(v), y = file_f32(v + 4), z = file_f32(v + 8);

        px[a] = x * m[0] + y * m[4] + z * m[8] + m[12];
        pz[a] = x * m[2] + y * m[6] + z * m[10] + m[14];
    }
    for (a = 0; a < point_count; a++) {
        for (b = a + 1; b < point_count; b++) {
            float dd = (px[a] - px[b]) * (px[a] - px[b]) + (pz[a] - pz[b]) * (pz[a] - pz[b]);

            if (dd > best) {
                best = dd;
                leaf[0] = px[a];
                leaf[1] = pz[a];
                leaf[2] = px[b];
                leaf[3] = pz[b];
            }
        }
    }
    return best > 100.0f * 100.0f && best < 3000.0f * 3000.0f;
}

/* KF4_AUTOMAP_DUMP=DIR: the browsed map's markers into DIR/automap_browse_NNN.txt, as dump_markers writes them,
   then its objects in use and its spawns with the state they were shown by */
static void dump_browsed(const AutomapScriptWorld *w, const MapTables *t, int slot, int visited) {
    const char *dir = getenv("KF4_AUTOMAP_DUMP");
    char path[1024];
    FILE *f;
    int i;

    if (dir == NULL) {
        return;
    }
    snprintf(path, sizeof(path), "%s/automap_browse_%03d.txt", dir, plan.map);
    f = fopen(path, "w");
    if (f == NULL) {
        return;
    }
    fprintf(f, "map %d: %dx%d cells from %d,%d, layout %08x, slot %d, visited %d\n", plan.map, plan.width, plan.height,
            (int)plan.x0, (int)plan.z0, layout_hash, slot, visited);
    for (i = 0; i < marker_count; i++) {
        char text[256], *c;

        snprintf(text, sizeof(text), "%s", markers[i].text);
        for (c = text; *c; c++) {
            if (*c == '\n') {
                *c = '|';
            }
        }
        fprintf(f, "%d kind %d flags %d at %d %d %d: %s\n", i, markers[i].kind, markers[i].flags, (int)markers[i].x,
                (int)markers[i].y, (int)markers[i].z, text);
    }
    for (i = 0; i < w->spawn_count; i++) {
        const EntitySpawnInfo *si = &t->spawns[i];

        fprintf(f, "spawn %d chr %d type %d talk %d flags %08x can_appear %d deaths %d at %d %d %d\n", i, si->type_id,
                spawn_params(si)->entity_type, si->talk_script_id, w->entity_flags[i],
                spawn_flags_can_appear(w->entity_flags[i], si, t->tag, i), Game_GetSpawnDeathCount(t->tag, i),
                (int)si->spawn_x, (int)si->spawn_y, (int)si->spawn_z);
    }
    for (i = 0; i < w->object_count; i++) {
        if (w->object_state[i] & MAP_OBJECT_IN_USE) {
            fprintf(f, "object %d state %08x\n", i, w->object_state[i]);
        }
    }
    fclose(f);
}

/* What a browsed map is shown from besides its file: its objects', spawns' and fixed items' state as the player
   would find them walking in now, and its records' (which the start script makes solid or not: command 150,
   ScriptCmd_SetPropSolid; MapSetup_RunForMap) */
static struct {
    AutomapScriptWorld w;
    MapTables t;
    int slot;    /* the game's slot that holds the map, -1 none */
    int visited; /* Game_IsMapVisited */
    u32 entity_flags[0x80], object_state[0x200];
    u8 item_placed[0x40], item_taken[0x40];
    f32 item_position[0x40][3];
} bw;

/* the state of the browsed map in `f`: bw, and its records' (f->record_hidden, f->record_move) */
static void browse_prepare(MapFile *f, int map) {
    const MapSlotUnk08 *h = f->header;
    int i;

    bw.slot = browse_cold ? -1 : loaded_slot(map);
    bw.visited = Game_IsMapVisited(map);
    bw.t.tag = map;
    bw.t.items = (const MapItemDef *)f->items;
    bw.t.item_count = h->item_count < 0x40 ? h->item_count : 0x40;
    bw.t.item_kinds = (const s16 *)f->item_kinds;
    bw.t.spawns = (const EntitySpawnInfo *)f->spawns;
    bw.t.spawn_count = h->spawn_count < 0x80 ? h->spawn_count : 0x80;
    memset(&bw.w, 0, sizeof(bw.w));
    memset(bw.item_placed, 0, sizeof(bw.item_placed));
    memset(bw.w.spawn_by_id, 0xFF, sizeof(bw.w.spawn_by_id));
    memset(bw.w.object_by_id, 0xFF, sizeof(bw.w.object_by_id));
    memset(bw.w.item_by_id, 0xFF, sizeof(bw.w.item_by_id));
    bw.w.script = f->script;
    bw.w.script_end = f->end;
    bw.w.map = map;
    bw.w.entity_flags = bw.entity_flags;
    bw.w.object_state = bw.object_state;
    bw.w.item_placed = bw.item_placed;
    bw.w.item_taken = bw.item_taken;
    bw.w.item_position = bw.item_position;

    /* the objects: MapObject_StartMap's state, or the live one */
    bw.w.object_count = h->object_count < 0x200 ? h->object_count : 0x200;
    for (i = 0; i < bw.w.object_count; i++) {
        const MapObjectUnk08 *src = (const MapObjectUnk08 *)(f->objects + i * 0x80);
        const MapObjectUnk04 *def = (const MapObjectUnk04 *)&map_object_defs[src->def_index * 0x80];
        int id = src->spawn_flags.bytes[2], in_use, on = 0, emptied = 0;

        if (id != 0 && bw.w.object_by_id[id] < 0) {
            bw.w.object_by_id[id] = (s16)i;
        }
        if (bw.slot >= 0) {
            bw.object_state[i] = (u32)map_slot_objects[bw.slot][i].state_flags;
            continue;
        }
        in_use = !((src->spawn_flags.flags >> 49) & 1);
        if (bw.visited) {
            in_use = Game_GetObjectActive(map, i);
            on = Game_GetObjectState(map, i);
            emptied = Game_GetObjectFlag(map, i, 0);
        } else if (def->kind == 8 || def->kind == 4 || def->kind == 0xD || def->kind == 0x10) {
            on = ((const MapObjectUnk08Words *)src)->kind_param_7 != 0;
        }
        if (Game_GetTriggerItem(map, id)) {
            on = 1;
        }
        bw.object_state[i] = (in_use ? MAP_OBJECT_IN_USE : 0) | (on ? MAP_OBJECT_ON : 0) | (emptied ? MAP_OBJECT_EMPTIED : 0);
    }
    /* the spawns: Entity_InitFromMapSpawns' flags (bit 7 unless the spawn's bits 59-60 are 2; 5 and 6), or the
       live ones */
    bw.w.spawn_count = bw.t.spawn_count;
    for (i = 0; i < bw.w.spawn_count; i++) {
        const EntitySpawnInfo *si = &bw.t.spawns[i];

        bw.w.spawn_by_id[(si->spawn_flags >> 32) & 0xFF] = (s16)i;
        if (bw.slot >= 0) {
            bw.entity_flags[i] = (u32)entity_pool[bw.slot][i].flags;
        } else {
            bw.entity_flags[i] = ENTITY_FLAG_OWN_THINK | ENTITY_FLAG_HANDLER_RUNS |
                              (((si->spawn_flags >> 59) & 3) == 2 ? 0 : ENTITY_FLAG_RESPAWNABLE);
        }
    }
    /* the fixed items: those the map has a model for (MapItem_InitMap), not taken as often as they may be */
    bw.w.item_count = bw.t.item_count;
    for (i = 0; i < bw.w.item_count; i++) {
        const MapItemDef *def = &bw.t.items[i];

        bw.w.item_by_id[def->slot_id] = (s16)i;
        bw.item_taken[i] = item_put_out(&bw.t, def->item) == 0 ||
                        (def->required_flag != 0 && map_item_pickup_counts[map & 0x7F][i] >= def->required_flag);
        bw.item_position[i][0] = def->position.x;
        bw.item_position[i][1] = def->position.y;
        bw.item_position[i][2] = def->position.z;
    }
    /* the records: solid or not (record_flags 0x40000000: map_triangles leaves them out), and where (a slot's
       records are moved by the slot's offset, Map_OffsetRecords, and some by the story: map 029's lift) */
    bw.w.record_count = h->record_count;
    bw.w.record_hidden = f->record_hidden = calloc(h->record_count + 1, 1);
    f->record_move = calloc(h->record_count + 1, sizeof(*f->record_move));
    memset(bw.w.record_by_prop, 0xFF, sizeof(bw.w.record_by_prop));
    for (i = 0; i < h->record_count && f->record_hidden != NULL && f->record_move != NULL; i++) {
        const uint8_t *rec = f->records + i * 0x80;
        int prop = rec[0x4E];

        if (prop != 0 && bw.w.record_by_prop[prop] < 0) {
            bw.w.record_by_prop[prop] = (s16)i;
        }
        f->record_hidden[i] = (file_u32(rec + 0x3C) & 0x40000000) != 0;
        if (bw.slot >= 0) {
            const char *live = map_slots[bw.slot].records;
            int k;

            f->record_hidden[i] = (((const MapRecordBounds *)(live + i * 0x80))->record_flags & 0x40000000) != 0;
            for (k = 0; k < 3; k++) {
                f->record_move[i][k] = (((const float *)(live + i * 0x80))[k] - file_f32(rec + 4 * k)) -
                                       (((const float *)live)[k] - file_f32(f->records + 4 * k));
            }
        }
    }
    if (f->record_hidden == NULL || f->record_move == NULL) {
        bw.w.record_count = 0;
    }
    if (bw.slot < 0) {
        setup_for_map(&bw.w, f);
    }
    Automap_RunStartScript(&bw.w);

}

/* the markers of the browsed map in hand, from its file and bw */
static void browse_markers(const MapFile *f, int map) {
    int i;

    marker_count = 0;
    exit_markers();
    for (i = 0; i < bw.w.object_count; i++) {
        const MapObjectUnk08 *src = (const MapObjectUnk08 *)(f->objects + i * 0x80);
        const MapObjectUnk04 *def = (const MapObjectUnk04 *)&map_object_defs[src->def_index * 0x80];
        float leaf[4];
        int has_leaf;

        if (!(bw.object_state[i] & MAP_OBJECT_IN_USE)) {
            continue;
        }
        has_leaf = is_door(def->kind) && file_door_leaf(f, src, leaf);
        object_marker(&bw.t, i, def, src, bw.object_state[i], src->position.x, src->position.y, src->position.z,
                      has_leaf ? leaf : NULL);
    }
    if (port_automap.items) {
        if (bw.slot >= 0) {
            /* the items lying in the bw.slot, moved back into the map's own frame (the bw.slot's records are moved by
               the same offset: Map_OffsetRecords) */
            const float *live = (const float *)map_slots[bw.slot].records;
            float offset[3];

            offset[0] = live[0] - file_f32(f->records + 0);
            offset[1] = live[1] - file_f32(f->records + 4);
            offset[2] = live[2] - file_f32(f->records + 8);
            item_markers(bw.slot, offset);
        } else {
            /* MapItem_RollFixedItems: a roll per group against the map's threshold, one per item against its
               chance; an item a script puts out is there whatever the rolls */
            for (i = 0; i < bw.w.item_count; i++) {
                const MapItemDef *def = &bw.t.items[i];
                int threshold = f->thresholds[def->group & 15], chance = def->chance;
                int percent = (threshold > 100 ? 100 : threshold) * (chance > 100 ? 100 : chance) / 100;
                char note[96];

                if (bw.item_taken[i] || (!bw.item_placed[i] && percent <= 0)) {
                    continue;
                }
                note[0] = 0;
                if (!bw.item_placed[i] && percent < 100) {
                    snprintf(note, sizeof(note), "(there by chance as you walk in: %d%%)", percent);
                }
                item_marker(&bw.t, def->item, def, bw.item_position[i][0], bw.item_position[i][1], bw.item_position[i][2],
                            note[0] ? note : NULL);
                if (note[0] && marker_count > 0) {
                    markers[marker_count - 1].flags |= PORT_AUTOMAP_MARK_MAYBE;
                }
            }
            /* the items dropped in the map (MapItem_InitMap drops them again) */
            for (i = 0; i < 16; i++) {
                const DroppedItemRecord *r = &dropped_item_records[map & 0x7F][i];

                if (r->used) {
                    item_marker(&bw.t, r->item, NULL, r->position[0], r->position[1], r->position[2], NULL);
                }
            }
        }
    }
    /* the people, by the flags their spawns would have as the player walks in; the enemies' places, faint */
    for (i = 0; i < bw.w.spawn_count; i++) {
        const EntitySpawnInfo *si = &bw.t.spawns[i];
        const EntityParams *p = spawn_params(si);
        int type = p->entity_type, record = si->type_id, later;
        float x = si->spawn_x, y = si->spawn_y, z = si->spawn_z;
        const char *hint = Automap_SpawnHint(map, i, record);
        u32 flags = bw.entity_flags[i];
        int there = spawn_flags_can_appear(flags, si, map, i) ||
                    ((flags & ENTITY_FLAG_LIVE) && !(flags & ENTITY_FLAG_DYING_MASK));
        PortAutomapMarker *m;
        char line[128];

        if ((type == 15 || type == 81) && si->talk_script_id != 0) {
            later = !there;
            if ((later && !port_automap.secrets) || (x == 0.0f && y == 0.0f && z == 0.0f) ||
                (!later && !seen_near(x, y, z))) {
                continue;
            }
            m = person_marker(i, si, p, x, y, z, later);
        } else {
            const char *creature;

            /* the dead Expedition Soldiers lying as set dressing (entity_prop: chr 93, variants 1 and 2) */
            if (!there || type == 15 || type == 81 || (record == 93 && (si->variant == 1 || si->variant == 2))) {
                continue;
            }
            if (!port_automap.enemies && !(hint != NULL && port_automap.hints && seen_near(x, y, z))) {
                continue;
            }
            m = new_marker(PORT_AUTOMAP_MARK_ENEMY, x, y, z);
            if (m == NULL) {
                return;
            }
            m->flags |= PORT_AUTOMAP_MARK_CHEAT | PORT_AUTOMAP_MARK_ABSENT;
            creature = Automap_CreatureName(record);
            if (creature != NULL) {
                add_line(m->text, sizeof(m->text), creature);
            } else {
                snprintf(line, sizeof(line), "Creature (#%d)", record);
                add_line(m->text, sizeof(m->text), line);
            }
            add_line(m->text, sizeof(m->text), "(where it comes into the world as you come near)");
            if (port_automap.items && p->drop_item != 0xFFFF && p->drop_item != 0 && p->drop_chance > 0) {
                char name[64];

                item_name(p->drop_item, name, sizeof(name));
                snprintf(line, sizeof(line), "May drop: %s (%d%%)", name, p->drop_chance);
                add_line(m->text, sizeof(m->text), line);
            }
        }
        if (m == NULL) {
            break;
        }
        hint_line(m, hint);
    }
    dump_browsed(&bw.w, &bw.t, bw.slot, bw.visited);
}

/* sets the SSE unit's mode (MXCSR), returns the one it had */
static uint32_t port_float_mode(uint32_t mode) {
#if defined(__x86_64__) || defined(__i386__)
    uint32_t saved = _mm_getcsr();

    _mm_setcsr(mode);
    return saved;
#else
    (void)mode;
    return 0;
#endif
}

/* the maps browsed, the last the one shown; none: the player's own */
#define MAX_BROWSED 32
typedef struct {
    int map;
    float x, y, z; /* where it is shown from: the way it was reached by, in its own frame */
} BrowseStop;
static BrowseStop browse_history[MAX_BROWSED];
static int browse_depth;
static PortAutomapPlayer browse_focus;


/* builds the browsed map `stop` (into `browsed`): 0 when its file cannot be read */
static int browse_build(const BrowseStop *stop) {
    PlanSource source;
    MapFile f;
    const char *name;
    uint32_t csr;

    if (!map_file_read(stop->map, &f)) {
        return 0;
    }
    /* the game's float arithmetic, as the player's map is built (the overlay draws in the host's): the same
       plan, cell for cell, so that its explored state matches it (layout_hash) */
    csr = port_float_mode(port_ee_mxcsr());
    view_swap(&browsed);
    browse_prepare(&f, stop->map);
    source.slot = -1;
    source.file = &f;
    build_plan(&source, stop->map, stop->x, stop->z);
    if (plan.map >= 0) {
        find_exits((const char *)f.records, f.header->record_count, (const char *)f.origins);
        scan_script(f.script, f.end);
        browse_markers(&f, stop->map);
    }
    view_swap(&browsed);
    port_float_mode(csr);
    map_file_free(&f);
    if (browsed.plan.map < 0) {
        return 0;
    }
    memset(&browse_focus, 0, sizeof(browse_focus));
    browse_focus.x = stop->x;
    browse_focus.y = stop->y;
    browse_focus.z = stop->z;
    browse_focus.map = stop->map;
    browse_focus.in_play = 1;
    name = port_teleport_map_name(stop->map);
    snprintf(browse_focus.title, sizeof(browse_focus.title), "%s", name[0] ? name : "Map");
    port_event("automap: browsing map %03d (%d markers)", stop->map, browsed.marker_count);
    return 1;
}

int port_automap_browse(int map, float x, float y, float z) {
    BrowseStop stop;

    if (!port_automap.enabled || plan.map < 0) {
        return 0;
    }
    if (map == player.map) { /* the way back into the player's own map: home */
        port_automap_browse_home();
        return 1;
    }
    if (!port_automap.reveal && !map_explored(map)) {
        return 0;
    }
    stop.map = map;
    stop.x = x;
    stop.y = y;
    stop.z = z;
    if (!browse_build(&stop)) {
        return 0;
    }
    if (browse_depth == MAX_BROWSED) {
        memmove(browse_history, browse_history + 1, sizeof(browse_history[0]) * (MAX_BROWSED - 1));
        browse_depth--;
    }
    browse_history[browse_depth++] = stop;
    return 1;
}

/* KF4_AUTOMAP_BROWSE's "self": the player's own map built as the browser builds a map in no slot, from its file,
   the save's tables and its start script (the tables hold its state as the last save or quick save wrote them), to
   check against the markers of the map he stands in */
int port_automap_browse_self_test(void) {
    BrowseStop stop;
    int ok;

    if (!port_automap.enabled || plan.map < 0) {
        return 0;
    }
    stop.map = plan.map;
    stop.x = player.x;
    stop.y = player.y;
    stop.z = player.z;
    browse_cold = 1;
    ok = browse_build(&stop);
    browse_cold = 0;
    if (ok && browse_depth < MAX_BROWSED) {
        browse_history[browse_depth++] = stop;
    }
    return ok;
}

void port_automap_browse_back(void) {
    while (browse_depth > 1) {
        browse_depth--;
        if (browse_build(&browse_history[browse_depth - 1])) {
            return;
        }
    }
    port_automap_browse_home();
}

void port_automap_browse_home(void) {
    browse_depth = 0;
}

int port_automap_browsing(void) {
    return port_automap.enabled && browse_depth > 0 && browsed.plan.map >= 0 ? browsed.plan.map : -1;
}

const PortAutomapPlan *port_automap_browse_plan(void) {
    return port_automap_browsing() >= 0 ? &browsed.plan : NULL;
}

int port_automap_browse_markers(const PortAutomapMarker **list) {
    *list = browsed.markers != NULL ? browsed.markers : browsed_markers;
    return port_automap_browsing() >= 0 ? browsed.marker_count : 0;
}

void port_automap_browse_focus(PortAutomapPlayer *out) {
    *out = browse_focus;
}

/* ---- The poll ------------------------------------------------------------------------------------- */
void Automap_Poll(void) {
    int slot = current_map_slot & 1, in_play;
    MapSlot *s = &map_slots[slot];

    if (!port_automap.enabled) {
        plan.map = -1;
        player.in_play = player.shown = 0;
        return;
    }
    port_automap_start();
    if (!s->loaded || s->header == NULL || s->records == NULL) {
        player.in_play = player.shown = 0;
        return;
    }
    in_play = Quick_WhyNotInPlay() == NULL;
    if (in_play && port_automap_new_game()) {
        /* a new game: nothing explored yet */
        Automap_Deserialize(NULL, 0);
        port_automap_mark_dirty();
        port_event("automap: a new game, nothing explored");
    }
    if (plan.map != s->tag || built_slot != slot || built_records != s->records) {
        Automap_Flush();
        build(slot);
        port_automap_mark_dirty();
        polls_since_build = 0;
    }
    if (plan.map < 0) {
        return;
    }
    polled_vblank = port_vblank_count();
    polls++;
    player.x = player_state.position.x;
    player.y = player_state.position.y;
    player.z = player_state.position.z;
    player.heading = player_state.rotation.y;
    player.map = plan.map;
    player.in_play = in_play;
    player.shown = !game_status.paused;
    {
        const char *name = port_teleport_map_name(plan.map);

        snprintf(player.title, sizeof(player.title), "%s", name[0] ? name : "Map");
    }
    if (in_play) {
        int i, j, cell = cell_of(player.x, player.z, &i, &j) ? j * plan.width + i : -1;

        walked();
        if (cell != last_reveal_cell || fabsf(player.heading - last_reveal_heading) > 0.3f || polls % 15 == 0) {
            if (reveal()) {
                plan.version++;
                port_automap_mark_dirty();
            }
            last_reveal_cell = cell;
            last_reveal_heading = player.heading;
        }
    }
    build_markers(slot);
    if (++polls_since_build == 30 || polls_since_build == 240) { /* again once the creatures near have come in */
        dump_markers(slot);
    }
    port_automap_save_live(0);
}

/* ---- The port's side reads these ------------------------------------------------------------------ */
const PortAutomapPlan *port_automap_plan(void) {
    return port_automap.enabled && plan.map >= 0 ? &plan : NULL;
}

int port_automap_markers(const PortAutomapMarker **list) {
    *list = markers;
    return port_automap.enabled && plan.map >= 0 ? marker_count : 0;
}

void port_automap_player(PortAutomapPlayer *out) {
    *out = player;
    /* polled within the last few vblanks (a game frame is two; a menu stops the polls) */
    if (port_vblank_count() > polled_vblank + 6) {
        out->in_play = 0;
        out->shown = 0;
    }
}
