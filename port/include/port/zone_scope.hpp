// A zone of the frame profiler (port/include/port/zones.h) from here to the end of the scope: begun now, ended at
// the scope's end if its begin was written (recording)
//
//     static const ZoneId draw_zone = port_zone_id("pc draw_batch", ZONE_WORK); // (once: at namespace scope)
//     ZoneScope zone(draw_zone);
#pragma once

extern "C" {
#include "port/zones.h"
}

class ZoneScope {
public:
    explicit ZoneScope(ZoneId id) : id_(id), begun_(port_zone_begin(id) != 0) {}
    ~ZoneScope() {
        if (begun_) {
            port_zone_end(id_);
        }
    }
    ZoneScope(const ZoneScope &) = delete;
    ZoneScope &operator=(const ZoneScope &) = delete;

private:
    ZoneId id_;
    bool begun_;
};
