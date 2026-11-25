/* stats_mod.c: the extended stats mod's switches (port/include/port/stats.h) and their keys in
   settings.json's "mods":

     "extended_stats": false,              the mod (--extended-stats turns it on for one run)
     "extended_stats_menus": true,         its lines in the game's own menus
     "extended_stats_panel": true,         the stats panel, on its key (F6)
     "extended_stats_damage": true,        the hit log
     "extended_stats_bar": true,           the health bar of the creature last hit (missing: as
                                           extended_stats_damage, which used to switch it)
     "extended_stats_durability": true,    the equipment's wear (the menus and the panel)
     "extended_stats_timers": true,        the ailments and timed effects running, on screen by the minimap
     "extended_stats_bar_place": "top"     where the target's health bar is: "top", "top_left" (under the HUD),
                                           "top_right" or "bottom"

   "extended_stats_status_screen" (the panel opening with the Status screen, which the menus' lines
   replaced) is no longer read, and is taken out of the file when it is written.

   The parts only count while the mod is on; off, nothing of it is drawn, and it never changes the game. */
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "port/stats.h"

PortStatsOptions port_stats_options = {0, 0, 1, 1, 1, 1, 1, 1, PORT_STATS_BAR_TOP}; /* on, forced, menus, panel,
                                                                                     damage, durability, timers, bar,
                                                                                     bar_place */

const char *const port_stats_bar_place_names[PORT_STATS_BAR_PLACE_COUNT] = {
    [PORT_STATS_BAR_TOP] = "top",
    [PORT_STATS_BAR_TOP_LEFT] = "top_left",
    [PORT_STATS_BAR_TOP_RIGHT] = "top_right",
    [PORT_STATS_BAR_BOTTOM] = "bottom",
};

static int panel_open;
/* What the player chose (settings.json, the player menu's Interface tab); --extended-stats leaves it as it was */
static int saved_on;

void port_stats_set_on(int on) {
    port_stats_options.on = saved_on = on != 0;
}

void port_stats_toggle_panel(void) {
    if (port_stats_options.on && port_stats_options.panel) {
        panel_open = !panel_open;
    }
}

int port_stats_panel_open(void) {
    return panel_open && port_stats_options.on && port_stats_options.panel;
}

void port_stats_set_panel_open(int open) {
    panel_open = open != 0;
}

static int read_bool(const cJSON *mods, const char *key, int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(mods, key);

    return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

static void read_bar_place(const cJSON *mods, PortStatsBarPlace *place) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(mods, "extended_stats_bar_place");
    int i;

    if (!cJSON_IsString(item)) {
        return;
    }
    for (i = 0; i < PORT_STATS_BAR_PLACE_COUNT; i++) {
        if (strcmp(item->valuestring, port_stats_bar_place_names[i]) == 0) {
            *place = (PortStatsBarPlace)i;
            return;
        }
    }
}

void port_stats_settings_read(const void *mods_json) {
    const cJSON *mods = (const cJSON *)mods_json;
    PortStatsOptions *o = &port_stats_options;

    saved_on = read_bool(mods, "extended_stats", saved_on);
    if (!o->forced) {
        o->on = saved_on;
    }
    o->menus = read_bool(mods, "extended_stats_menus", o->menus);
    o->panel = read_bool(mods, "extended_stats_panel", o->panel);
    o->damage = read_bool(mods, "extended_stats_damage", o->damage);
    o->durability = read_bool(mods, "extended_stats_durability", o->durability);
    o->timers = read_bool(mods, "extended_stats_timers", o->timers);
    o->bar = read_bool(mods, "extended_stats_bar", o->damage);
    read_bar_place(mods, &o->bar_place);
}

static void put_item(cJSON *mods, const char *key, cJSON *item) {

    if (cJSON_GetObjectItemCaseSensitive(mods, key) != NULL) {
        cJSON_ReplaceItemInObjectCaseSensitive(mods, key, item);
    } else {
        cJSON_AddItemToObject(mods, key, item);
    }
}

static void put(cJSON *mods, const char *key, int value) {
    put_item(mods, key, cJSON_CreateBool(value));
}

void port_stats_settings_write(void *mods_json) {
    cJSON *mods = (cJSON *)mods_json;
    PortStatsOptions *o = &port_stats_options;

    put(mods, "extended_stats", saved_on);
    put(mods, "extended_stats_menus", o->menus);
    put(mods, "extended_stats_panel", o->panel);
    cJSON_DeleteItemFromObjectCaseSensitive(mods, "extended_stats_status_screen");
    put(mods, "extended_stats_damage", o->damage);
    put(mods, "extended_stats_durability", o->durability);
    put(mods, "extended_stats_timers", o->timers);
    put(mods, "extended_stats_bar", o->bar);
    put_item(mods, "extended_stats_bar_place", cJSON_CreateString(port_stats_bar_place_names[o->bar_place]));
}
