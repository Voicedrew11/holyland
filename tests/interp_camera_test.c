/* port/platform/interp_camera.c: which two frames' cameras are a cut apart (a fast spin of the mouse is not), and the
   camera between them.

     interp_camera_test */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "port/interp_camera.h"

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

static double wrap(double a) {
    const double pi = 3.14159265358979323846;

    return a - 2 * pi * floor((a + pi) / (2 * pi));
}

/* A camera at the eye looking by (yaw, pitch), as the game builds its view (mouse_lead_turn_view on the identity) */
static InterpCamera camera(double yaw, double pitch, double x, double y, double z) {
    InterpCamera c;
    MouseTurn turn = {yaw, pitch};
    int i, j;

    memset(&c, 0, sizeof(c));
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            c.r[i][j] = i == j;
        }
    }
    mouse_lead_turn_view(c.r, turn);
    c.eye[0] = x;
    c.eye[1] = y;
    c.eye[2] = z;
    for (i = 0; i < 3; i++) {
        c.t[i] = -(c.r[i][0] * x + c.r[i][1] * y + c.r[i][2] * z);
    }
    return c;
}

static MouseTurn spent(double yaw, double pitch) {
    MouseTurn m = {wrap(yaw), wrap(pitch)};
    return m;
}

int main(void) {
    InterpCamera a = camera(0.3, 0.1, 10, 20, 30);
    MouseTurn none = {0, 0};

    /* the mouse's turn, however fast, is not a cut */
    {
        InterpCamera b = camera(0.3 + 1.2, 0.1, 10, 20, 30);
        check("a 1.2 rad turn the mouse made: not a cut", !interp_cameras_cut(&a, spent(5, 0), &b, spent(6.2, 0)));
        check("the same turn the mouse did not make: a cut", interp_cameras_cut(&a, none, &b, none));
    }
    {
        InterpCamera b = camera(0.3 - 1.5, 0.1 + 0.4, 12, 20, 30);
        check("a quarter turn and a pitch the mouse made, walking: not a cut",
              !interp_cameras_cut(&a, spent(1, 0.2), &b, spent(1 - 1.5, 0.6)));
    }
    {
        InterpCamera b = camera(0.3 + 0.5, 0.1, 10, 20, 30);
        check("a 0.5 rad turn of the camera's own: not a cut", !interp_cameras_cut(&a, none, &b, none));
    }
    {
        InterpCamera b = camera(0.3 + 1.2 + 1.0, 0.1, 10, 20, 30);
        check("1 rad more than the mouse's 1.2: a cut", interp_cameras_cut(&a, spent(0, 0), &b, spent(1.2, 0)));
    }
    {
        InterpCamera b = camera(0.3 + 2.8, 0.1, 10, 20, 30);
        check("2.8 rad in all, though the mouse's: a cut", interp_cameras_cut(&a, spent(0, 0), &b, spent(2.8, 0)));
    }
    {
        InterpCamera b = camera(0.3, 0.1, 10 + 401, 20, 30);
        check("a move of 401: a cut", interp_cameras_cut(&a, none, &b, none));
    }
    {
        /* the spent totals wrapped round [-pi, pi) between the two frames */
        InterpCamera b = camera(0.3 + 1.0, 0.1, 10, 20, 30);
        check("the mouse's turn across the wrap: not a cut", !interp_cameras_cut(&a, spent(3.0, 0), &b, spent(4.0, 0)));
    }

    /* the camera between */
    {
        InterpCamera b = camera(0.3 + 1.2, 0.1 + 0.2, 30, 20, 10);
        InterpCamera c0 = interp_camera_between(&a, &b, 0), c1 = interp_camera_between(&a, &b, 1);
        InterpCamera h = interp_camera_between(&a, &b, 0.5);
        double ta = interp_turn_between(a.r, h.r), tb = interp_turn_between(h.r, b.r), all = interp_turn_between(a.r, b.r);

        check("between at 0 is the first camera", interp_turn_between(a.r, c0.r) < 1e-9 && fabs(c0.t[0] - a.t[0]) < 1e-9);
        check("between at 1 is the second", interp_turn_between(b.r, c1.r) < 1e-9 && fabs(c1.t[2] - b.t[2]) < 1e-9);
        check("half way: half the turn each side", fabs(ta - all / 2) < 1e-9 && fabs(tb - all / 2) < 1e-9);
        check("half way: the eye half way", fabs(h.eye[0] - 20) < 1e-9 && fabs(h.eye[2] - 20) < 1e-9);
    }

    printf("%d of %d checks passed\n", checked - failed, checked);
    return failed != 0;
}
