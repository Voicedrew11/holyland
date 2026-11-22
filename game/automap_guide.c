/* automap_guide.c: PC-only. What the automap (game/automap.c, docs/port/AUTOMAP.md) says that the game's data
   does not: the creatures' names, and hand-written guide notes for locks, puzzles and odd objects. The map's
   cheats stand in for an online guide, so these tables grow as the game is played through; everything else
   the map shows comes from the player's disc and the game's memory as it runs.

   - Creatures: the game names no monster anywhere in its data. These are the names of Prima's official
     strategy guide (2004) as the community's creature list has them (cleanhit.wiki, KF4:Creatures), placed on
     the chr.prm records by hit points, experience, resistances and drops (decomp/docs/GAME_DATA.md,
     "Monsters"; the decomp's enemy/ files carry the same names). A record not listed shows as
     "Creature (#record)".
   - Hints: by map and map object (its index in the map's object list, the map file's order: the same every
     visit; the full map's hover label shows it with the hints cheat, e.g. "#45"), or by map and entity spawn
     (the spawn record's index, "spawn #8"). Shown with the hints cheat. Write what the player needs to know,
     not the game's text. */
#include <stddef.h>

#include "port/automap.h"

typedef struct {
    int record;
    const char *name;
} CreatureName;

static const CreatureName creature_names[] = {
    {0, "Maneater"},
    {2, "Green Slime"},
    {3, "Poison Slime"},
    {5, "Scorpion"},
    {6, "Spider"},
    {7, "Giant Spider"},
    {8, "Acid Crawler (large)"},
    {9, "Dragon Fly"},
    {10, "Blood Bat"},
    {11, "Beetle (small)"},
    {12, "Critter"},
    {13, "Snake"},
    {14, "Lava Lizard"},
    {15, "Fire Lord"},
    {16, "Salamander"},
    {17, "Widdacona"},
    {18, "Phagos"},
    {19, "Beetle (very large)"},
    {20, "Giant Beetle"},
    {21, "Living Statue"},
    {22, "Stone Guardian"},
    {23, "Sentinel"},
    {24, "Living Armor"},
    {25, "Golden Statue"},
    {27, "Sol"},
    {30, "Watcher"},
    {31, "Slasher"},
    {32, "Giant"},
    {33, "Malformed Giant"},
    {34, "Earth King"},
    {35, "Genan"},
    {36, "Fadin"},
    {37, "Master Fadin"},
    {38, "Lava Face"},
    {39, "Beetle (white)"},
    {40, "Makara"},
    {41, "Aqualizard"},
    {42, "Beetle (medium)"},
    {43, "Seafang"},
    {44, "Widda (large)"},
    {45, "Widda Queen"},
    {46, "Widda Archer (large)"},
    {47, "Skull"},
    {49, "Dark Flower"},
    {50, "Skeleton Soldier (round shield)"},
    {51, "Skeleton Soldier (square shield)"},
    {52, "Skeleton Soldier (axe)"},
    {53, "Skeleton Archer (small)"},
    {54, "Skeleton"},
    {55, "Zombie"},
    {56, "Living Dead"},
    {57, "Ancient King"},
    {58, "High Skeleton (small)"},
    {59, "Minion"},
    {60, "Bell"},
    {61, "Mummy"},
    {62, "Ancient Mummy"},
    {64, "Giant Leader"},
    {66, "Malice"},
    {67, "Fire Elemental"},
    {68, "Earth Elemental"},
    {69, "Wind Elemental"},
    {70, "Water Elemental"},
    {71, "Dark Succubus"},
    {72, "Dark Knight"},
    {73, "Dark Soldier"},
    {74, "Fire Genie"},
    {75, "Greater Demon"},
    {76, "Dark Archer"},
    {78, "Dark Sorcerer"},
    {80, "Ice Golem"},
    {81, "Larva"},
    {82, "Dark Puppet"},
    {83, "Dark Bird"},
    {84, "Para Stool"},
    {85, "Reaper"},
    {86, "Crow"},
    {87, "Stool"},
    {91, "Eye pedestal"},
    {92, "Expedition Archer"},
    {93, "Expedition Soldier"},
    {97, "Lee Maynor"},
    {98, "Widda (armored)"},
    {99, "The Dark One"},
    {150, "Skeleton Soldier"},
    {151, "Blue Spider"},
    {152, "Beetle"},
    {154, "Skeleton with a hammer"},
    {155, "Mutant Wall"},
    {156, "Mutant Floor"},
    {157, "Mutant Ceiling"},
    {158, "Lava Bird"},
    {159, "Onyx"},
    {160, "Widda"},
    {161, "Widda Archer (small)"},
    {164, "High Skeleton (large)"},
    {165, "Acid Crawler (small)"},
    {167, "High Skeleton Soldier"},
    {168, "Skeleton Archer (large)"},
    {171, "Widda Archer"},
};

const char *Automap_CreatureName(int record) {
    size_t i;

    for (i = 0; i < sizeof(creature_names) / sizeof(creature_names[0]); i++) {
        if (creature_names[i].record == record) {
            return creature_names[i].name;
        }
    }
    return NULL;
}

/* ---- Whose page a person's variant record is -------------------------------------------------------
   A person's name is read from his Recollection page (record 100 + page), or his model's; a variant record
   with neither is listed here with the record whose page is his. */
static const struct {
    int record, page_record;
} person_pages[] = {
    {162, 104}, /* Lynn Genne later in the story (map 004, by her father's grave) */
};

int Automap_PersonPage(int record) {
    size_t i;

    for (i = 0; i < sizeof(person_pages) / sizeof(person_pages[0]); i++) {
        if (person_pages[i].record == record) {
            return person_pages[i].page_record;
        }
    }
    return -1;
}

/* ---- What a hazard is -----------------------------------------------------------------------------
   Hazard effect 4 (CollisionMesh.hazard_strips, Collision_GetSurfaceEffect) is lava on every map that has it,
   and the automap draws it as lava (PORT_AUTOMAP_FIRE) everywhere. The game has no "sea" kind of it: the
   player standing in it (Player_CollideWithMap sets ground_effect_kind) takes 10 HP a frame
   (Player_UpdateStatus), moves and sinks slowly (Player_Move's row 4 of player_top_speed and player_gravity:
   6 and 0.03), and steps with footstep sound 0x27; an entity in it takes up to 50 less its fire resistance
   (EntityCollision_ApplySurfaceEffect: EntityParams.resist_fire). Water is another strip list
   (CollisionMesh.water_strips: swimming, status bits 38-39), drawn blue; poisoned or corroding water is
   hazard effects 1-3 or 5 laid on water strips.
   The maps with effect 4, each looked at in the game standing in it (November 2026): 000 Village Ruins (the
   lava north and east of the save point's ruin), 018 and 019 The Forge, 024 The Cavern of Lava, 025 The Fire
   Lord's Domain, 099 Palace of Oath Exterior (the pools beside the path), 102 The Ancient City, Level 3,
   East, and 116 The Treasury. 000 and 099 were listed here as the sea until then, and drawn as deep water. */

/* ---- Hints ------------------------------------------------------------------------------------------
   AUTOMAP_OBJECT: a map object (map, object index); AUTOMAP_SPAWN: an entity (map, spawn index). */
enum { AUTOMAP_OBJECT, AUTOMAP_SPAWN };

typedef struct {
    int what;
    int map;
    int index; /* -1: every one of its kind on the map (the spawns: every spawn of that chr record, see below) */
    int record; /* AUTOMAP_SPAWN with index -1: the chr.prm record */
    const char *hint;
} AutomapHint;

static const AutomapHint hints[] = {
    /* The King's Throne, Upper (006): the eight eye pedestals before the room whose ceiling drops (Prima's
       walkthrough). The pedestal spawned in mode 1 is the one that does not follow the player. */
    {AUTOMAP_SPAWN, 6, -1, 91, "Find the eye that does not follow you and strike it: that closes all the eyes "
                               "(the pressure plate past them drops the ceiling of the room beyond)"},
    /* The King's Throne, Lower (007): the bell (record 60) */
    {AUTOMAP_SPAWN, 7, -1, 60, "Strike or shoot the bell to bring down the skeletons around it"},
};

const char *Automap_ObjectHint(int map, int object) {
    size_t i;

    for (i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        if (hints[i].what == AUTOMAP_OBJECT && hints[i].map == map && hints[i].index == object) {
            return hints[i].hint;
        }
    }
    return NULL;
}

const char *Automap_SpawnHint(int map, int spawn, int record) {
    size_t i;

    for (i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        if (hints[i].what == AUTOMAP_SPAWN && hints[i].map == map &&
            (hints[i].index == spawn || (hints[i].index < 0 && hints[i].record == record))) {
            return hints[i].hint;
        }
    }
    return NULL;
}
