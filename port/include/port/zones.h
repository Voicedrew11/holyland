/* The frame profiler's record (docs/port/PROFILING.md, "Frames"): what every thread and the GPU were doing,
   section by section ("zones"), on one clock, picture by picture.

   A lane is one timeline: each thread that times a zone has one of its own (made at its first zone), and each
   source of GPU times has one (port_zone_lane_make: its times put on the CPU's clock). A lane is written by one
   thread only, into a ring of its own, without a lock; port/include/port/zone_record.hpp reads every lane's ring
   on a thread of its own ("kf4-profiler") and works out frames: the spans between two pictures shown
   (port_zone_picture).

   A zone's name is registered once (port_zone_id) and its id kept in a static. Zones nest: an end closes the zone
   its lane began last. Off (nothing recording: port_zones_set_recording), a begin or an end costs one relaxed load.

   port/platform/zones.c, in kf4_profile with port/include/port/profile.h. Any thread may call any of these. */
#ifndef PORT_ZONES_H
#define PORT_ZONES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef uint16_t ZoneId; /* 0: none */
#define ZONE_NONE 0
#define ZONE_IDS_MOST 4096
#define ZONE_NAME_MOST 112

/* What a zone's time is */
typedef enum {
    ZONE_WORK, /* the thread busy */
    ZONE_WAIT, /* the thread blocked: on another thread, the GPU, the screen or a deadline */
    ZONE_GPU,  /* the GPU busy (a GPU lane's) */
    ZONE_KINDS
} ZoneKind;

/* Recording: begins and ends kept. A count of who wants it (the panel, KF4_PROFILE, a CSV): on while above 0 */
int port_zones_recording(void);
void port_zones_want(int more); /* +1 or -1 */

/* The id of a zone named name (the same name, the same id: the kind of its first registration kept), 0 when the
   table is full. Any thread, at any time (static initialisers too). */
ZoneId port_zone_id(const char *name, ZoneKind kind);
/* Its name and kind; "" and ZONE_WORK for an id not registered */
const char *port_zone_name(ZoneId id);
ZoneKind port_zone_kind(ZoneId id);
/* Ids registered: 1 to port_zone_count() */
int port_zone_count(void);

/* The calling thread's lane: a zone begun at now. 1 when written (recording): only then is it ended, with
   port_zone_end, so that a zone begun before recording turned on is never ended in the record. C++: ZoneScope
   (port/include/port/zone_scope.hpp). */
int port_zone_begin(ZoneId id);
void port_zone_end(ZoneId id);

/* The calling thread's lane: a zone from start_ns to now, told after it ended (a compile timed by a library). Its
   lane must have written nothing since start_ns: the zone goes after what it has. */
void port_zone_after(ZoneId id, uint64_t start_ns);

/* A picture shown, at now: a frame ends here and the next begins */
void port_zone_picture(void);

/* ---- Lanes ---- */
#define ZONE_LANES_MOST 64
#define ZONE_LANE_NAME 48
typedef struct ZoneLane ZoneLane;
/* A lane of the caller's own (a GPU's: gpu non-zero), for times it puts on the CPU's clock itself; NULL when there
   is no room. Never freed. */
ZoneLane *port_zone_lane_make(const char *name, int gpu);
/* At ns (port_profile_now_ns' clock), on lane, in the order of their times */
void port_zone_lane_begin_at(ZoneLane *lane, ZoneId id, uint64_t ns);
void port_zone_lane_end_at(ZoneLane *lane, ZoneId id, uint64_t ns);

/* ---- Reading (zone_record.hpp's) ---- */
typedef struct {
    uint64_t ns;
    ZoneId id;
    uint8_t end; /* 1: an end; 0: a begin */
} ZoneEvent;

/* The lanes made so far: 0 to port_zone_lanes()-1; a lane's name ("kf4-pc", "GPU kf4-pc") and whether it is a GPU's */
int port_zone_lanes(void);
const char *port_zone_lane_name(int lane);
int port_zone_lane_is_gpu(int lane);
/* Lane's events written since *cursor, the oldest first, into events (at most most); *cursor moved past them.
   *lost (may be NULL): events written over before they were read, skipped. Their count. */
size_t port_zone_lane_read(int lane, uint64_t *cursor, ZoneEvent *events, size_t most, uint64_t *lost);
/* The pictures shown since *cursor, their times into ns (at most most); their count */
size_t port_zone_pictures_read(uint64_t *cursor, uint64_t *ns, size_t most);

#ifdef __cplusplus
}
#endif

#endif
