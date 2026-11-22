/* The scene renderer's depth modes (port/include/port/scene_depth.h): every mode has a name and a label, each name
   (and old name) finds its own mode again, anything else finds none, and the developers' variables win over the
   setting.

     scene_depth_test */
#include <stdio.h>
#include <string.h>

#include "port/scene_depth.h"

static int failures = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "scene_depth_test: %s\n", what);
        failures++;
    }
}

int main(void) {
    for (int m = 0; m < PORT_SCENE_DEPTH_COUNT; m++) {
        const PortSceneDepth *d = port_scene_depth(m);
        check(d != NULL && d->name != NULL && d->label != NULL, "a mode without a name or label");
        if (d != NULL && d->name != NULL) {
            check(port_scene_depth_from_name(d->name) == m, "a name that does not find its own mode");
        }
        if (d != NULL && d->old_name != NULL) {
            check(port_scene_depth_from_name(d->old_name) == m, "an old name that does not find its own mode");
        }
    }
    /* settings.json and --scene-depth spell them so, and so before the menu's levels (still read) */
    check(port_scene_depth_from_name("high") == PORT_SCENE_DEPTH_EXACT, "\"high\" is not PORT_SCENE_DEPTH_EXACT");
    check(port_scene_depth_from_name("medium") == PORT_SCENE_DEPTH_NEARER, "\"medium\" is not PORT_SCENE_DEPTH_NEARER");
    check(port_scene_depth_from_name("low") == PORT_SCENE_DEPTH_GPU, "\"low\" is not PORT_SCENE_DEPTH_GPU");
    check(port_scene_depth_from_name("unsettled") == PORT_SCENE_DEPTH_UNSETTLED,
          "\"unsettled\" is not PORT_SCENE_DEPTH_UNSETTLED");
    check(port_scene_depth_from_name("exact") == PORT_SCENE_DEPTH_EXACT, "\"exact\" is not PORT_SCENE_DEPTH_EXACT");
    check(port_scene_depth_from_name("nearer") == PORT_SCENE_DEPTH_NEARER, "\"nearer\" is not PORT_SCENE_DEPTH_NEARER");
    check(port_scene_depth_from_name("gpu") == PORT_SCENE_DEPTH_GPU, "\"gpu\" is not PORT_SCENE_DEPTH_GPU");
    check(PORT_SCENE_DEPTH_EXACT == 0, "the default (a settings.json without it, kf4-gsreplay) is not exact");
    check(port_scene_depth(-1) == NULL && port_scene_depth(PORT_SCENE_DEPTH_COUNT) == NULL, "a mode past the table");
    check(port_scene_depth_from_name(NULL) == -1 && port_scene_depth_from_name("") == -1 &&
              port_scene_depth_from_name("High") == -1 && port_scene_depth_from_name("settle") == -1,
          "an unknown name finds a mode");

    /* the developers' variables over the setting */
    for (int m = 0; m < PORT_SCENE_DEPTH_COUNT; m++) {
        check(port_scene_depth_with_env(m, NULL, NULL) == m, "no variables: not the setting's mode");
        check(port_scene_depth_with_env(m, "gpu", NULL) == PORT_SCENE_DEPTH_GPU, "KF4_SCN_DEPTH=gpu: not gpu");
        check(port_scene_depth_with_env(m, "gpu", "nearer") == PORT_SCENE_DEPTH_GPU,
              "KF4_SCN_DEPTH=gpu with KF4_SCN_SETTLE: not gpu");
        check(port_scene_depth_with_env(m, NULL, "0") == PORT_SCENE_DEPTH_UNSETTLED, "KF4_SCN_SETTLE=0: not unsettled");
        check(port_scene_depth_with_env(m, NULL, "nearer") == PORT_SCENE_DEPTH_NEARER,
              "KF4_SCN_SETTLE=nearer: not nearer");
        check(port_scene_depth_with_env(m, "exact", "1") == m, "other values: not the setting's mode");
    }
    check(port_scene_depth_with_env(-1, NULL, NULL) == PORT_SCENE_DEPTH_EXACT &&
              port_scene_depth_with_env(PORT_SCENE_DEPTH_COUNT, NULL, NULL) == PORT_SCENE_DEPTH_EXACT,
          "a mode past the table: not exact");
    if (failures == 0) {
        printf("scene_depth_test: %d modes ok\n", PORT_SCENE_DEPTH_COUNT);
    }
    return failures == 0 ? 0 : 1;
}
