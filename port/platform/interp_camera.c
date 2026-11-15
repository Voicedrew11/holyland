/* The cameras of the pictures between: port/include/port/interp_camera.h */
#include <math.h>
#include <string.h>

#include "port/interp_camera.h"

static void quat_from(const double m[3][3], double q[4]) {
    double tr = m[0][0] + m[1][1] + m[2][2];

    if (tr > 0) {
        double s = sqrt(tr + 1.0) * 2;
        q[3] = 0.25 * s;
        q[0] = (m[2][1] - m[1][2]) / s;
        q[1] = (m[0][2] - m[2][0]) / s;
        q[2] = (m[1][0] - m[0][1]) / s;
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        double s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q[3] = (m[2][1] - m[1][2]) / s;
        q[0] = 0.25 * s;
        q[1] = (m[0][1] + m[1][0]) / s;
        q[2] = (m[0][2] + m[2][0]) / s;
    } else if (m[1][1] > m[2][2]) {
        double s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q[3] = (m[0][2] - m[2][0]) / s;
        q[0] = (m[0][1] + m[1][0]) / s;
        q[1] = 0.25 * s;
        q[2] = (m[1][2] + m[2][1]) / s;
    } else {
        double s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q[3] = (m[1][0] - m[0][1]) / s;
        q[0] = (m[0][2] + m[2][0]) / s;
        q[1] = (m[1][2] + m[2][1]) / s;
        q[2] = 0.25 * s;
    }
}

static void matrix_from(const double q[4], double m[3][3]) {
    double x = q[0], y = q[1], z = q[2], w = q[3];

    m[0][0] = 1 - 2 * (y * y + z * z);
    m[0][1] = 2 * (x * y - z * w);
    m[0][2] = 2 * (x * z + y * w);
    m[1][0] = 2 * (x * y + z * w);
    m[1][1] = 1 - 2 * (x * x + z * z);
    m[1][2] = 2 * (y * z - x * w);
    m[2][0] = 2 * (x * z - y * w);
    m[2][1] = 2 * (y * z + x * w);
    m[2][2] = 1 - 2 * (x * x + y * y);
}

double interp_turn_between(const double a[3][3], const double b[3][3]) {
    double trace = 0, c;
    int i, j;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            trace += a[i][j] * b[i][j]; /* (the trace of b a^T) */
        }
    }
    c = (trace - 1) / 2;
    return acos(c > 1 ? 1 : c < -1 ? -1 : c);
}

static double wrap_rad(double a) {
    const double pi = 3.14159265358979323846;

    return a - 2 * pi * floor((a + pi) / (2 * pi));
}

int interp_cameras_cut(const InterpCamera *a, MouseTurn mouse_a, const InterpCamera *b, MouseTurn mouse_b) {
    double move = 0, mouse_turned[3][3];
    MouseTurn mouse = {wrap_rad(mouse_b.yaw_rad - mouse_a.yaw_rad), wrap_rad(mouse_b.pitch_rad - mouse_a.pitch_rad)};
    int i;

    for (i = 0; i < 3; i++) {
        move += (b->eye[i] - a->eye[i]) * (b->eye[i] - a->eye[i]);
    }
    if (move > INTERP_MAX_MOVE * INTERP_MAX_MOVE || interp_turn_between(a->r, b->r) > INTERP_MOST_TURN) {
        return 1;
    }
    memcpy(mouse_turned, a->r, sizeof mouse_turned);
    mouse_lead_turn_view(mouse_turned, mouse);
    return interp_turn_between(mouse_turned, b->r) > INTERP_MAX_TURN;
}

InterpCamera interp_camera_between(const InterpCamera *a, const InterpCamera *b, double f) {
    InterpCamera between;
    double qa[4], qb[4], q[4], dot = 0, angle;
    int i, row;

    quat_from(a->r, qa);
    quat_from(b->r, qb);
    for (i = 0; i < 4; i++) {
        dot += qa[i] * qb[i];
    }
    if (dot < 0) {
        for (i = 0; i < 4; i++) {
            qb[i] = -qb[i];
        }
        dot = -dot;
    }
    dot = dot > 1 ? 1 : dot;
    angle = acos(dot); /* half the turn */
    if (angle < 1e-6) {
        for (i = 0; i < 4; i++) {
            q[i] = qa[i] + (qb[i] - qa[i]) * f;
        }
    } else {
        double wa = sin((1 - f) * angle) / sin(angle), wb = sin(f * angle) / sin(angle);

        for (i = 0; i < 4; i++) {
            q[i] = qa[i] * wa + qb[i] * wb;
        }
    }
    {
        double n = sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);

        for (i = 0; i < 4; i++) {
            q[i] /= n;
        }
    }
    matrix_from(q, between.r);
    for (i = 0; i < 3; i++) {
        between.eye[i] = a->eye[i] + (b->eye[i] - a->eye[i]) * f;
    }
    for (row = 0; row < 3; row++) {
        between.t[row] = -(between.r[row][0] * between.eye[0] + between.r[row][1] * between.eye[1] + between.r[row][2] * between.eye[2]);
    }
    return between;
}
