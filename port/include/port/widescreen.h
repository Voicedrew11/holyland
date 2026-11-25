/* The widescreen picture's shapes (port_options.widescreen): the PS2's 4:3 (off), the common screens' shapes, and
   Fill, the window's own. By number and by name (settings.json's display "widescreen", --widescreen=, the player
   menu's Display tab, KF4_SET_AT's widescreen). A shape is one entry in the table below.

   The game draws its 3D world into the PS2's 640 x 448 frame with a field of view as wide as the shape, and its 2D
   (laid out for 4:3) is squeezed to keep its shape (port/platform/gs.c); the window shows the frame at the shape.
   Header only: the pure maths here are unit tested (tests/widescreen_test.c); the shape in use now, which for Fill
   follows the window, is port_picture_shape() (port/platform/gs.c). */
#ifndef PORT_WIDESCREEN_H
#define PORT_WIDESCREEN_H

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    PORT_SHAPE_4_3,   /* widescreen off: the PS2's (settings.json's false) */
    PORT_SHAPE_16_9,  /* --widescreen's, and settings.json's true from before the shapes */
    PORT_SHAPE_16_10,
    PORT_SHAPE_21_9,  /* 64:27, 2560 x 1080's (3440 x 1440 is 43:18, a hair wider: Fill fits it exactly) */
    PORT_SHAPE_32_9,
    PORT_SHAPE_FILL,  /* the window's own shape, kept between 4:3 and 32:9 */
    PORT_SHAPE_COUNT
};

/* A picture's shape, in lowest terms */
typedef struct {
    int across, down;
} PortShape;

typedef struct {
    const char *name;  /* settings.json's and the command line's */
    const char *label; /* the player menu's */
    PortShape shape;   /* (Fill's: the one used with no window, 16:9) */
} PortShapeChoice;

static inline const PortShapeChoice *port_shape_choice(int choice) {
    static const PortShapeChoice choices[PORT_SHAPE_COUNT] = {
        {"off", "Off", {4, 3}},          /* PORT_SHAPE_4_3 (in the enum's order) */
        {"16:9", "16:9", {16, 9}},       /* PORT_SHAPE_16_9 */
        {"16:10", "16:10", {8, 5}},      /* PORT_SHAPE_16_10 */
        {"21:9", "21:9", {64, 27}},      /* PORT_SHAPE_21_9 */
        {"32:9", "32:9", {32, 9}},       /* PORT_SHAPE_32_9 */
        {"fill", "Fill Screen", {16, 9}}, /* PORT_SHAPE_FILL */
    };
    return choice >= 0 && choice < PORT_SHAPE_COUNT ? &choices[choice] : NULL;
}

/* The choice named name, or -1 for none */
static inline int port_shape_from_name(const char *name) {
    for (int c = 0; name != NULL && c < PORT_SHAPE_COUNT; c++) {
        if (strcmp(port_shape_choice(c)->name, name) == 0) {
            return c;
        }
    }
    return -1;
}

static inline int port_shape_gcd(int a, int b) {
    while (b != 0) {
        int r = a % b;
        a = b;
        b = r;
    }
    return a;
}

/* The picture's shape for a choice, in a window of window_w x window_h pixels (0 x 0: none known). Fill's is the
   window's, kept between 4:3 (a narrower window shows 4:3 with bars above and below) and 32:9. */
static inline PortShape port_shape_of(int choice, int window_w, int window_h) {
    const PortShapeChoice *c = port_shape_choice(choice);
    PortShape s;

    if (c == NULL) {
        c = port_shape_choice(PORT_SHAPE_4_3);
    }
    if (choice != PORT_SHAPE_FILL || window_w <= 0 || window_h <= 0) {
        return c->shape;
    }
    if ((long long)window_w * 3 < (long long)window_h * 4) {
        return port_shape_choice(PORT_SHAPE_4_3)->shape;
    }
    if ((long long)window_w * 9 > (long long)window_h * 32) {
        return port_shape_choice(PORT_SHAPE_32_9)->shape;
    }
    int g = port_shape_gcd(window_w, window_h);
    s.across = window_w / g;
    s.down = window_h / g;
    return s;
}

/* A 4:3 width's in the shape's, as a fraction squeeze_num / squeeze_den (3/4 at 16:9, 1 at 4:3): the 2D's squeeze
   and the game's projection's across */
static inline long long port_shape_squeeze_num(PortShape s) {
    return 4LL * s.down;
}
static inline long long port_shape_squeeze_den(PortShape s) {
    return 3LL * s.across;
}

/* x (in any unit) squeezed about anchor, rounded toward anchor as the GS's 12.4 vertices are */
static inline int port_shape_squeeze_x(PortShape s, int x, int anchor) {
    return anchor + (int)((long long)(x - anchor) * port_shape_squeeze_num(s) / port_shape_squeeze_den(s));
}

/* The renderer's scale across for its scale down (--native-widescreen): the smallest whole number at least the
   shape's width over 4:3's times it, so that a GS pixel's width on the screen is never more than a target pixel's
   height; but no more than the renderer's frame holds (PORT_GS_FRAME_PIXELS: 24 = 6 across x 4 down), so the widest
   shapes at the highest scales draw a little softer across. */
static inline int port_shape_scale_x(PortShape s, int scale) {
    long long num = 3LL * s.across * scale, den = 4LL * s.down;
    int x = (int)((num + den - 1) / den);
    int most = scale > 0 ? 24 / scale : 1;

    if (x > most) {
        x = most;
    }
    return x < scale ? scale : x;
}

/* ---- The HUD's area (port_options.widescreen_hud) ----------------------------------------------------------------
   Where in a wide picture the HUD lays out: the game's gauges, the stats bar and the quick slots against its left and
   right edges (port/platform/gs.c), the minimap, its timers and the frame-rate counter in its corners (the overlay).
   An area at most so wide, centred: on a picture narrower than the area, the picture's edges. The values keep
   settings.json's and the menu's from before the areas (0 the edges, 1 centred). */
enum {
    PORT_HUD_EDGES,   /* the picture's edges ("corner") */
    PORT_HUD_CENTRED, /* the 4:3 middle ("centred"), where the menus are */
    PORT_HUD_16_9,
    PORT_HUD_21_9,
    PORT_HUD_COUNT
};

typedef struct {
    const char *name;  /* settings.json's "widescreen_hud" */
    const char *label; /* the player menu's */
    PortShape shape;   /* the widest the area is ({0, 0}: the whole picture) */
} PortHudArea;

static inline const PortHudArea *port_hud_area(int area) {
    static const PortHudArea areas[PORT_HUD_COUNT] = {
        {"corner", "Screen Edges", {0, 0}},  /* PORT_HUD_EDGES (in the enum's order) */
        {"centred", "4:3 (Centred)", {4, 3}}, /* PORT_HUD_CENTRED */
        {"16:9", "16:9", {16, 9}},           /* PORT_HUD_16_9 */
        {"21:9", "21:9", {64, 27}},          /* PORT_HUD_21_9 */
    };
    return area >= 0 && area < PORT_HUD_COUNT ? &areas[area] : NULL;
}

/* The area named name, or -1 for none */
static inline int port_hud_area_from_name(const char *name) {
    for (int a = 0; name != NULL && a < PORT_HUD_COUNT; a++) {
        if (strcmp(port_hud_area(a)->name, name) == 0) {
            return a;
        }
    }
    return -1;
}

/* The HUD area's width as a fraction of a picture of that shape: 1 for the edges or an area as wide as the picture
   or wider, 3/4 for 4:3 in 16:9 */
static inline double port_shape_hud_fraction(PortShape picture, int area) {
    const PortHudArea *a = port_hud_area(area);

    if (a == NULL || a->shape.across == 0) {
        return 1.0;
    }
    double fraction = ((double)a->shape.across * picture.down) / ((double)a->shape.down * picture.across);
    return fraction < 1.0 ? fraction : 1.0;
}

#ifdef __cplusplus
}
#endif

#endif
