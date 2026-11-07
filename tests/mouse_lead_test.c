/* port/platform/mouse_lead.c: instant mouse look's turn of the view, against the game's own way of building it
   (libvu0's rotations, as GameLoop_SetCameraMatrices calls them), and the turn a picture between is given.

     mouse_lead_test */
#include <math.h>
#include <stdio.h>

#include "libvu0.h"
#include "port/mouse_lead.h"

#define PI 3.14159265358979323846

static int failed, checked;

static void check(const char *what, int ok) {
    checked++;
    if (!ok) {
        failed++;
        printf("FAIL %s\n", what);
    } else {
        printf("ok   %s\n", what);
    }
}

static int near(double a, double b, double within) {
    return fabs(a - b) <= within;
}

/* The game's world-to-view rotation for a camera at (yaw, pitch), in r[row][col] as frame_interp.c reads the
   matrix's fields (its rows m[col] are the transform's columns) */
static void game_view(double yaw, double pitch, double r[3][3]) {
    float m[4][4];
    int row, col;

    sceVu0UnitMatrix(m);
    sceVu0RotMatrixY(m, m, (float)-yaw);
    sceVu0RotMatrixX(m, m, (float)-pitch);
    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            r[row][col] = m[col][row];
        }
    }
}

static int same_view(const double a[3][3], const double b[3][3], double within) {
    int row, col;

    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            if (!near(a[row][col], b[row][col], within)) {
                return 0;
            }
        }
    }
    return 1;
}

static int turned_to(double yaw, double pitch, MouseTurn turn, double to_yaw, double to_pitch) {
    double r[3][3], want[3][3];

    game_view(yaw, pitch, r);
    mouse_lead_turn_view(r, turn);
    game_view(to_yaw, to_pitch, want);
    return same_view(r, want, 2e-5);
}

static void test_turn_view(void) {
    MouseTurn none = {0, 0}, right = {0.3, 0}, up = {0, 0.2}, both = {-0.25, -0.15}, far_up = {0, 0.9};

    check("no turn leaves the view", turned_to(0.7, 0.1, none, 0.7, 0.1));
    check("a yaw is the game's yaw plus it", turned_to(0.7, 0.1, right, 1.0, 0.1));
    check("a pitch is the game's pitch plus it", turned_to(0.7, 0.1, up, 0.7, 0.3));
    check("both at once", turned_to(-2.0, 0.4, both, -2.25, 0.25));
    check("across the wrap of the yaw", turned_to(PI - 0.1, 0, right, -PI + 0.2, 0));
    check("the pitch stops at the game's limit", turned_to(0.2, 0.5, far_up, 0.2, MOUSE_LEAD_PITCH_LIMIT_RAD));
    {
        MouseTurn far_down = {0.1, -3.0};

        check("and at the other limit", turned_to(0.2, -0.5, far_down, 0.3, -MOUSE_LEAD_PITCH_LIMIT_RAD));
    }
    {
        /* a camera already past the limit (a cutscene's) is not pulled back into it */
        MouseTurn more = {0, 0.1};

        check("past the limit it stays", turned_to(0, 1.3, more, 0, 1.3));
        check("but turns back toward it", turned_to(0, 1.3, (MouseTurn){0, -0.1}, 0, 1.2));
    }
}

static MouseTurn turn(double yaw, double pitch) {
    MouseTurn t = {yaw, pitch};

    return t;
}

/* The game's thread from a fresh start. At each vblank v the mouse is read (the hand: what was spent and what
   waits); a picture at time p is given the hand at p - MOUSE_LEAD_DELAY_VBLANKS. */
static void test_ahead(void) {
    MouseTurn shown, drawn, ahead;
    const double d = MOUSE_LEAD_DELAY_VBLANKS;

    ahead = port_mouse_lead_ahead(turn(0, 0), turn(0, 0), 0.5, 100.0);
    check("before any reading: nothing ahead", ahead.yaw_rad == 0 && ahead.pitch_rad == 0);

    /* the hand turns 0.01 a vblank from vblank 100 on; the game spends it every second vblank */
    shown = port_mouse_lead_spent_total();
    for (int v = 100; v <= 110; v++) {
        if (v % 2 == 0 && v > 100) {
            port_mouse_lead_spent(turn(0.02, 0.004));
        }
        port_mouse_lead_waiting(turn(v % 2 == 0 ? 0 : 0.01, v % 2 == 0 ? 0 : 0.002), (uint64_t)v);
    }
    /* now spent: 5 x 0.02 = 0.1 (by vblank 110); the hand at vblank v is 0.01 (v - 100) */
    drawn = port_mouse_lead_spent_total();
    check("spent as given", near(drawn.yaw_rad, 0.1, 1e-12) && near(drawn.pitch_rad, 0.02, 1e-12));
    ahead = port_mouse_lead_ahead(shown, shown, 0.0, 105.0 + d);
    check("a picture at a reading's time + the delay: that reading's hand", near(ahead.yaw_rad, 0.05, 1e-12));
    ahead = port_mouse_lead_ahead(shown, shown, 0.0, 105.25 + d);
    check("between readings: mixed", near(ahead.yaw_rad, 0.0525, 1e-12) && near(ahead.pitch_rad, 0.0105, 1e-12));
    ahead = port_mouse_lead_ahead(shown, drawn, 0.25, 107.5 + d);
    check("less the camera between's mix of the frames' spent",
          near(ahead.yaw_rad, 0.075 - 0.25 * 0.1, 1e-12) && near(ahead.pitch_rad, 0.015 - 0.25 * 0.02, 1e-12));
    ahead = port_mouse_lead_ahead(shown, shown, 0.0, 130.0);
    check("past the newest reading: the newest, not carried on", near(ahead.yaw_rad, 0.10, 1e-12));
    ahead = port_mouse_lead_ahead(shown, shown, 0.0, 50.0);
    check("before the oldest kept: the oldest", near(ahead.yaw_rad, 0.11 - 0.01 * MOUSE_LEAD_READINGS, 1e-12));
    {
        double last = -1;
        int even = 1;

        for (int i = 0; i <= 16; i++) {
            ahead = port_mouse_lead_ahead(shown, shown, 0.0, 106.0 + i * 0.25 + d);
            if (last >= 0 && !near(ahead.yaw_rad - last, 0.0025, 1e-12)) {
                even = 0;
            }
            last = ahead.yaw_rad;
        }
        check("pictures four a vblank: the turn moves on the same in each", even);
    }

    /* a second reading at the same vblank replaces it (the hand put back to the spent, outside play) */
    port_mouse_lead_waiting(turn(0, 0), 110);
    ahead = port_mouse_lead_ahead(shown, shown, 0.0, 110.0 + d);
    check("the same vblank read again: the newer", near(ahead.yaw_rad, 0.10, 1e-12));

    /* a save state loaded: an earlier vblank, the readings after it gone */
    port_mouse_lead_waiting(turn(0.03, 0), 40);
    ahead = port_mouse_lead_ahead(drawn, drawn, 1.0, 105.0);
    check("readings from an earlier time start again", near(ahead.yaw_rad, 0.03, 1e-12));

    /* around the wrap: totals near pi, a turn across it */
    for (int i = 0; i < 30; i++) {
        port_mouse_lead_spent(turn(0.1, 0));
    }
    shown = port_mouse_lead_spent_total();
    port_mouse_lead_spent(turn(0.3, 0));
    drawn = port_mouse_lead_spent_total();
    port_mouse_lead_waiting(turn(0.1, 0), 41);
    check("totals kept in [-pi, pi)", drawn.yaw_rad >= -PI && drawn.yaw_rad < PI);
    ahead = port_mouse_lead_ahead(shown, drawn, 0.5, 41.0 + d);
    check("across the wrap: half of B's turn and what waits", near(ahead.yaw_rad, 0.15 + 0.1, 1e-9));

    /* many turns: the totals stay exact enough, through the mark's floats too */
    for (int i = 0; i < 1000000; i++) {
        port_mouse_lead_spent(turn(0.0123, -0.0007));
    }
    shown = port_mouse_lead_spent_total();
    port_mouse_lead_waiting(turn(0.01, 0.002), 42);
    ahead = port_mouse_lead_ahead(shown, shown, 0.5, 42.0 + d);
    check("after a million turns, as exact", near(ahead.yaw_rad, 0.01, 1e-9) && near(ahead.pitch_rad, 0.002, 1e-9));
    {
        MouseTurn as_float = {(float)shown.yaw_rad, (float)shown.pitch_rad};

        ahead = port_mouse_lead_ahead(as_float, as_float, 0.5, 42.0 + d);
        check("through the mark's floats: within a millionth of a radian", near(ahead.yaw_rad, 0.01, 1e-6));
    }
}

int main(void) {
    test_turn_view();
    test_ahead();
    printf("%d of %d checks failed\n", failed, checked);
    return failed != 0;
}
