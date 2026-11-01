/* Instant mouse look (port/include/port/mouse_lead.h) */
#include <math.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

#include "port/mouse_lead.h"

#define PI 3.14159265358979323846
#define TWO_PI (2.0 * PI)

#ifdef _WIN32
static SRWLOCK hand_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&hand_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&hand_lock)
#else
static pthread_mutex_t hand_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&hand_lock)
#define UNLOCK() pthread_mutex_unlock(&hand_lock)
#endif

typedef struct {
    uint64_t vblank;
    MouseTurn hand; /* spent_total and what waited then */
} Reading;

static MouseTurn spent_total; /* the game's thread's */
/* under hand_lock: the readings, oldest first; none until the first */
static Reading readings[MOUSE_LEAD_READINGS];
static int reading_count;

/* An angle in [-pi, pi) */
static double wrap(double a) {
    return a - TWO_PI * floor((a + PI) / TWO_PI);
}

static MouseTurn turn_sum(MouseTurn a, MouseTurn b) {
    MouseTurn sum = {wrap(a.yaw_rad + b.yaw_rad), wrap(a.pitch_rad + b.pitch_rad)};

    return sum;
}

void port_mouse_lead_spent(MouseTurn spent) {
    spent_total = turn_sum(spent_total, spent);
}

void port_mouse_lead_waiting(MouseTurn waiting, uint64_t vblank) {
    Reading reading = {vblank, turn_sum(spent_total, waiting)};

    LOCK();
    if (reading_count > 0 && readings[reading_count - 1].vblank > vblank) {
        reading_count = 0; /* (a save state loaded: an earlier time, the readings after it gone) */
    }
    if (reading_count > 0 && readings[reading_count - 1].vblank == vblank) {
        readings[reading_count - 1] = reading;
    } else {
        if (reading_count == MOUSE_LEAD_READINGS) {
            memmove(readings, readings + 1, sizeof(readings[0]) * (MOUSE_LEAD_READINGS - 1));
            reading_count--;
        }
        readings[reading_count++] = reading;
    }
    UNLOCK();
}

MouseTurn port_mouse_lead_spent_total(void) {
    return spent_total;
}

static double mixed(double a, double b, double f) {
    return wrap(a + f * wrap(b - a));
}

/* The hand at `at` (in vblanks), mixed between the readings about it; the oldest before them, the newest after.
   Under hand_lock, with a reading. */
static MouseTurn hand_at(double at) {
    MouseTurn hand;
    int i;

    if (at <= (double)readings[0].vblank) {
        return readings[0].hand;
    }
    for (i = 1; i < reading_count; i++) {
        const Reading *a = &readings[i - 1], *b = &readings[i];

        if (at <= (double)b->vblank) {
            double f = (at - (double)a->vblank) / (double)(b->vblank - a->vblank);

            hand.yaw_rad = mixed(a->hand.yaw_rad, b->hand.yaw_rad, f);
            hand.pitch_rad = mixed(a->hand.pitch_rad, b->hand.pitch_rad, f);
            return hand;
        }
    }
    return readings[reading_count - 1].hand;
}

static double ahead_of(double hand, double shown, double drawn, double t) {
    return wrap(hand - mixed(shown, drawn, t));
}

MouseTurn port_mouse_lead_ahead(MouseTurn spent_shown, MouseTurn spent_drawn, double t, double picture_vblank) {
    MouseTurn hand, ahead = {0, 0};
    int read;

    LOCK();
    read = reading_count > 0;
    if (read) {
        hand = hand_at(picture_vblank - MOUSE_LEAD_DELAY_VBLANKS);
    }
    UNLOCK();
    if (!read) {
        return ahead; /* (before the first reading: nothing waits) */
    }
    ahead.yaw_rad = ahead_of(hand.yaw_rad, spent_shown.yaw_rad, spent_drawn.yaw_rad, t);
    ahead.pitch_rad = ahead_of(hand.pitch_rad, spent_shown.pitch_rad, spent_drawn.pitch_rad, t);
    return ahead;
}

/* ---- The view --------------------------------------------------------------------------------- */

/* libvu0's rotations (sceVu0RotMatrixX/Y), in r[row][col] */
static void rot_x(double angle, double m[3][3]) {
    double c = cos(angle), s = sin(angle);

    m[0][0] = 1, m[0][1] = 0, m[0][2] = 0;
    m[1][0] = 0, m[1][1] = c, m[1][2] = -s;
    m[2][0] = 0, m[2][1] = s, m[2][2] = c;
}

static void rot_y(double angle, double m[3][3]) {
    double c = cos(angle), s = sin(angle);

    m[0][0] = c, m[0][1] = 0, m[0][2] = s;
    m[1][0] = 0, m[1][1] = 1, m[1][2] = 0;
    m[2][0] = -s, m[2][1] = 0, m[2][2] = c;
}

static void multiply(const double a[3][3], const double b[3][3], double out[3][3]) {
    int row, col;

    for (row = 0; row < 3; row++) {
        for (col = 0; col < 3; col++) {
            out[row][col] = a[row][0] * b[0][col] + a[row][1] * b[1][col] + a[row][2] * b[2][col];
        }
    }
}

/* The view's pitch: r = RotX(-pitch) RotY(-yaw), whose row 2, column 1 is -sin(pitch) */
static double view_pitch(const double r[3][3]) {
    double s = -r[2][1];

    return asin(s < -1 ? -1 : s > 1 ? 1 : s);
}

void mouse_lead_turn_view(double r[3][3], MouseTurn turn) {
    double pitch = view_pitch(r);
    double low = pitch < -MOUSE_LEAD_PITCH_LIMIT_RAD ? pitch : -MOUSE_LEAD_PITCH_LIMIT_RAD;
    double high = pitch > MOUSE_LEAD_PITCH_LIMIT_RAD ? pitch : MOUSE_LEAD_PITCH_LIMIT_RAD;
    double to = pitch + turn.pitch_rad;
    double x[3][3], y[3][3], half[3][3];

    to = to < low ? low : to > high ? high : to;
    rot_x(-(to - pitch), x);
    rot_y(-turn.yaw_rad, y);
    multiply(r, y, half);
    multiply(x, half, r);
}
