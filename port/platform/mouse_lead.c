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
    double vblank;  /* when it was read, in vblanks */
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

void port_mouse_lead_waiting(MouseTurn waiting, double at_vblanks) {
    Reading reading = {at_vblanks, turn_sum(spent_total, waiting)};

    LOCK();
    if (reading_count > 0 && readings[reading_count - 1].vblank > at_vblanks + 1.0) {
        reading_count = 0; /* (a save state loaded: an earlier time, the readings after it gone) */
    }
    if (reading_count > 0 && readings[reading_count - 1].vblank >= at_vblanks) {
        readings[reading_count - 1].hand = reading.hand;
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
    int lo = 0, hi = reading_count - 1;

    if (at <= readings[0].vblank) {
        return readings[0].hand;
    }
    if (at >= readings[hi].vblank) {
        return readings[hi].hand;
    }
    while (hi - lo > 1) { /* readings[lo].vblank < at <= readings[hi].vblank */
        int mid = (lo + hi) / 2;

        if (readings[mid].vblank < at) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    {
        const Reading *a = &readings[lo], *b = &readings[hi];
        double f = (at - a->vblank) / (b->vblank - a->vblank);

        hand.yaw_rad = mixed(a->hand.yaw_rad, b->hand.yaw_rad, f);
        hand.pitch_rad = mixed(a->hand.pitch_rad, b->hand.pitch_rad, f);
    }
    return hand;
}

MouseTurn port_mouse_lead_hand(double picture_vblank) {
    MouseTurn hand = {0, 0};

    LOCK();
    if (reading_count > 0) {
        hand = hand_at(picture_vblank - MOUSE_LEAD_DELAY_VBLANKS);
    }
    UNLOCK();
    return hand;
}

double port_mouse_lead_newest_vblanks(void) {
    double at;

    LOCK();
    at = reading_count > 0 ? readings[reading_count - 1].vblank : -1;
    UNLOCK();
    return at;
}

MouseTurn port_mouse_lead_since(MouseTurn hand, double at_vblanks) {
    MouseTurn since = {0, 0};

    LOCK();
    if (reading_count > 0) {
        MouseTurn there = hand_at(at_vblanks);

        since.yaw_rad = wrap(there.yaw_rad - hand.yaw_rad);
        since.pitch_rad = wrap(there.pitch_rad - hand.pitch_rad);
    }
    UNLOCK();
    return since;
}

static double ahead_of(double hand, double shown, double drawn, double t) {
    return wrap(hand - mixed(shown, drawn, t));
}

MouseTurn port_mouse_lead_ahead(MouseTurn spent_shown, MouseTurn spent_drawn, double t, double picture_vblank) {
    MouseTurn hand = {0, 0}, ahead = {0, 0};
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

void mouse_lead_turn_picture(float move[12], float mid[12], MouseTurn turn) {
    double r[3][3], turned[3][3], d[3][3], t[3], m[3][3], mt[3];
    int i, j, k;

    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            r[i][j] = turned[i][j] = mid[i * 4 + j];
            m[i][j] = move[i * 4 + j] + (i == j);
        }
        t[i] = mid[i * 4 + 3];
        mt[i] = move[i * 4 + 3];
    }
    mouse_lead_turn_view(turned, turn);
    /* the turn in the picture's view, about the eye: d = turned r^T (the eye stays: d t is the translation) */
    for (i = 0; i < 3; i++) {
        for (j = 0; j < 3; j++) {
            d[i][j] = 0;
            for (k = 0; k < 3; k++) {
                d[i][j] += turned[i][k] * r[j][k];
            }
        }
    }
    for (i = 0; i < 3; i++) {
        double dt = 0, dmt = 0;

        for (k = 0; k < 3; k++) {
            dt += d[i][k] * t[k];
            dmt += d[i][k] * mt[k];
        }
        for (j = 0; j < 3; j++) {
            double dm = 0;

            for (k = 0; k < 3; k++) {
                dm += d[i][k] * m[k][j];
            }
            mid[i * 4 + j] = (float)turned[i][j];
            move[i * 4 + j] = (float)(dm - (i == j));
        }
        mid[i * 4 + 3] = (float)dt;
        move[i * 4 + 3] = (float)dmt;
    }
}
