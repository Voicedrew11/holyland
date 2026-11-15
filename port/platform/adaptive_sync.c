/* Adaptive sync's cap under the screen's rate (port/include/port/adaptive_sync.h) */
#include "port/adaptive_sync.h"

int adaptive_sync_cap_hz(int refresh_hz) {
    if (refresh_hz <= 0) {
        return 0;
    }
    /* the room grows with the rate: a picture's unevenness is a share of a refresh, and a refresh is shorter */
    return refresh_hz - (refresh_hz * refresh_hz + 3599) / 3600;
}
