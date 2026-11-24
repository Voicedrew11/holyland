/* The smallest Holyland mod with code: it says when things happen, counts the game's frames through a hook on one of
   the game's own functions, and answers a key. Build: docs/port/MODDING.md, "Building one". */
#include "holyland_mod.h"

static const HlApi *api;
static HlMod *self;
static void (*original_input_update)(void);
static unsigned long inputs;

/* Input_Update, the game's reading of the pad each frame, as it was, counted */
static void counted_input_update(void) {
    inputs++;
    original_input_update();
}

static int on_event(HlMod *mod, HlEvent event, void *data, void *user) {
    static unsigned long frames;

    (void)user;
    switch (event) {
    case HL_EVENT_START:
        api->log(mod, "started: Holyland %s, the game's status at %p", api->holyland_version(), api->symbol("game_status"));
        break;
    case HL_EVENT_FRAME:
        if (++frames % 300 == 0) {
            api->log(mod, "%lu frames, Input_Update called %lu times", frames, inputs);
        }
        if (api->key_pressed(mod, "greet")) {
            api->log(mod, "hello (%s pressed)%s", api->key_name(mod, "greet"), api->setting(mod, "loud") ? "!!!" : "");
        }
        break;
    case HL_EVENT_TITLE:
        api->log(mod, "the title");
        break;
    case HL_EVENT_NEW_GAME:
        api->log(mod, "a new game");
        break;
    case HL_EVENT_SAVE_WRITTEN:
    case HL_EVENT_SAVE_LOADED:
        api->log(mod, "save %s %s", ((HlSave *)data)->key, event == HL_EVENT_SAVE_WRITTEN ? "written" : "loaded");
        break;
    default:
        break;
    }
    return 0;
}

HL_EXPORT int hl_mod_init(const HlApi *given, HlMod *mod) {
    api = given;
    self = mod;
    if (api->major != HL_API_MAJOR) {
        return 0;
    }
    api->log(mod, "hello from %s (data in %s)", api->mod_folder(mod), api->data_folder(mod));
    if (!api->hook(mod, "Input_Update", (void *)counted_input_update, (void **)&original_input_update)) {
        api->log(mod, "could not hook Input_Update");
    }
    api->on(mod, HL_EVENT_START, on_event, 0);
    api->on(mod, HL_EVENT_FRAME, on_event, 0);
    api->on(mod, HL_EVENT_TITLE, on_event, 0);
    api->on(mod, HL_EVENT_NEW_GAME, on_event, 0);
    api->on(mod, HL_EVENT_SAVE_WRITTEN, on_event, 0);
    api->on(mod, HL_EVENT_SAVE_LOADED, on_event, 0);
    return HL_API_MAJOR;
}
