/* The widescreen shapes (port/include/port/widescreen.h): every shape has a name that finds it again, Fill follows
   the window within 4:3 and 32:9, and the squeeze and the renderer's scale across are 16:9's as they were before
   the shapes (three quarters; 2, 3, 4 and 6 across).

     widescreen_test */
#include <stdio.h>
#include <string.h>

#include "port/widescreen.h"

static int failures = 0;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "widescreen_test: %s\n", what);
        failures++;
    }
}

static int same(PortShape s, int across, int down) {
    return s.across == across && s.down == down;
}

int main(void) {
    for (int c = 0; c < PORT_SHAPE_COUNT; c++) {
        const PortShapeChoice *choice = port_shape_choice(c);
        check(choice != NULL && choice->name != NULL && choice->label != NULL, "a shape without a name or label");
        if (choice != NULL && choice->name != NULL) {
            check(port_shape_from_name(choice->name) == c, "a name that does not find its own shape");
        }
        if (choice != NULL) {
            check(port_shape_gcd(choice->shape.across, choice->shape.down) == 1, "a shape not in lowest terms");
        }
    }
    check(port_shape_choice(-1) == NULL && port_shape_choice(PORT_SHAPE_COUNT) == NULL, "a shape past the table");
    check(port_shape_from_name(NULL) == -1 && port_shape_from_name("") == -1 && port_shape_from_name("16x9") == -1,
          "an unknown name finds a shape");
    check(strcmp(port_shape_choice(PORT_SHAPE_4_3)->name, "off") == 0, "PORT_SHAPE_4_3 is not \"off\"");

    /* the fixed shapes ignore the window */
    check(same(port_shape_of(PORT_SHAPE_4_3, 1920, 1080), 4, 3), "off is not 4:3");
    check(same(port_shape_of(PORT_SHAPE_16_9, 640, 480), 16, 9), "16:9 follows the window");
    check(same(port_shape_of(PORT_SHAPE_16_10, 0, 0), 8, 5), "16:10 is not 8:5");
    check(same(port_shape_of(-3, 1920, 1080), 4, 3), "an unknown choice is not 4:3");

    /* Fill: the window's, in lowest terms, kept between 4:3 and 32:9; 16:9 with no window */
    check(same(port_shape_of(PORT_SHAPE_FILL, 1920, 1080), 16, 9), "Fill at 1920 x 1080");
    check(same(port_shape_of(PORT_SHAPE_FILL, 3440, 1440), 43, 18), "Fill at 3440 x 1440");
    check(same(port_shape_of(PORT_SHAPE_FILL, 1280, 1024), 4, 3), "Fill narrower than 4:3");
    check(same(port_shape_of(PORT_SHAPE_FILL, 1080, 1920), 4, 3), "Fill on a tall window");
    check(same(port_shape_of(PORT_SHAPE_FILL, 10000, 1000), 32, 9), "Fill wider than 32:9");
    check(same(port_shape_of(PORT_SHAPE_FILL, 0, 0), 16, 9), "Fill with no window");

    /* the squeeze: 16:9's three quarters, rounded toward the anchor as (x - anchor) * 3 / 4 was */
    PortShape wide = port_shape_of(PORT_SHAPE_16_9, 0, 0);
    for (int x = 0x6000; x <= 0xA000; x += 7) {
        check(port_shape_squeeze_x(wide, x, 0x8000) == 0x8000 + (x - 0x8000) * 3 / 4, "16:9's squeeze changed");
        check(port_shape_squeeze_x(wide, x, 0x6C00) == 0x6C00 + (x - 0x6C00) * 3 / 4, "16:9's edge squeeze changed");
    }
    check(port_shape_squeeze_x(port_shape_of(PORT_SHAPE_4_3, 0, 0), 0x9400, 0x8000) == 0x9400, "4:3 squeezes");
    check(port_shape_squeeze_x(port_shape_of(PORT_SHAPE_32_9, 0, 0), 0x9400, 0x8000) == 0x8000 + 0x1400 * 3 / 8,
          "32:9's squeeze is not three eighths");
    check((float)port_shape_squeeze_num(wide) / (float)port_shape_squeeze_den(wide) == 0.75f,
          "16:9's projection is not exactly 0.75f");

    /* the renderer's scale across: 16:9's as before, never below the scale, never past the frame's 24 */
    static const int wide_x[] = {0, 2, 3, 4, 6};
    for (int s = 1; s <= 4; s++) {
        check(port_shape_scale_x(wide, s) == wide_x[s], "16:9's scale across changed");
        check(port_shape_scale_x(port_shape_of(PORT_SHAPE_4_3, 0, 0), s) == s, "4:3 draws wider");
        for (int c = 0; c < PORT_SHAPE_COUNT; c++) {
            int x = port_shape_scale_x(port_shape_of(c, 0, 0), s);
            check(x >= s && x * s <= 24, "a scale across outside the frame");
        }
    }
    check(port_shape_scale_x(port_shape_of(PORT_SHAPE_32_9, 0, 0), 2) == 6, "32:9 at 2x is not 6 across");
    check(port_shape_scale_x(port_shape_of(PORT_SHAPE_32_9, 0, 0), 4) == 6, "32:9 at 4x not held to the frame");

    /* the HUD's areas: names; the edges whole; an area narrower than the picture centred in it, else the edges */
    for (int a = 0; a < PORT_HUD_COUNT; a++) {
        const PortHudArea *area = port_hud_area(a);
        check(area != NULL && port_hud_area_from_name(area->name) == a, "a HUD area whose name does not find it");
    }
    check(PORT_HUD_EDGES == 0 && PORT_HUD_CENTRED == 1, "the HUD areas lost the values of Corner and Centred");
    check(port_hud_area_from_name("corner") == PORT_HUD_EDGES && port_hud_area_from_name("centred") == PORT_HUD_CENTRED,
          "settings.json's \"corner\" or \"centred\" from before is not read");
    check(port_hud_area(PORT_HUD_COUNT) == NULL && port_hud_area_from_name("middle") == -1, "a HUD area past the table");
    PortShape ultra = port_shape_of(PORT_SHAPE_32_9, 0, 0);
    check(port_shape_hud_fraction(ultra, PORT_HUD_EDGES) == 1.0, "the edges are not the whole picture");
    check(port_shape_hud_fraction(ultra, PORT_HUD_16_9) == 0.5, "16:9 in 32:9 is not half");
    check(port_shape_hud_fraction(ultra, PORT_HUD_CENTRED) == 0.375, "4:3 in 32:9 is not three eighths");
    check(port_shape_hud_fraction(wide, PORT_HUD_CENTRED) == 0.75, "4:3 in 16:9 is not the squeeze");
    check(port_shape_hud_fraction(wide, PORT_HUD_21_9) == 1.0, "21:9 in 16:9 is not the edges");
    check(port_shape_hud_fraction(port_shape_of(PORT_SHAPE_16_10, 0, 0), PORT_HUD_16_9) == 1.0,
          "16:9 in 16:10 is not the edges");
    check(port_shape_hud_fraction(wide, -1) == 1.0, "an unknown area is not the edges");

    if (failures == 0) {
        printf("widescreen_test: ok\n");
    }
    return failures != 0;
}
