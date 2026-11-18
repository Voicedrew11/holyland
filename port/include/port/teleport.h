/* Teleport (port/platform/teleport.c): a debugging aid that puts the player in any map, at a place read
   from the map's own file, keeping the game's progress. It is a quick load of a save image made from
   the game as it is now, with the map and the position changed (port/include/port/quicksave.h): the
   game restarts the session on that map, as loading a save does, and puts the player on the floor
   under the place.

   The places are those tools/map_tour.py visits: the map's start (where a map change without a link
   puts the player), the map's fixed items that lie on a floor, then the middles of the biggest floor
   triangles. */
#ifndef PORT_TELEPORT_H
#define PORT_TELEPORT_H

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_TELEPORT_PLACES 24

typedef struct {
    float position[3];
    float heading;
    char label[32]; /* "start", "item 3", "floor 12" (a record) */
} PortTeleportPlace;

/* The maps on the disc (KF4.DAT's NNN.map), in order: how many, and the n'th one's number */
int port_teleport_map_count(void);
int port_teleport_map(int n);
/* The map's name in the decomp's docs/GAME_DATA.md (Prima's guide), or "" */
const char *port_teleport_map_name(int map);
/* The places in the map (read from the disc the first time a map is asked for): how many, at most
   PORT_TELEPORT_PLACES, written to places */
int port_teleport_places(int map, PortTeleportPlace *places);

/* The way into a dead-end map: one whose records never ask for a neighbouring map (040, the Dark
   Reality's Conclusion, and 18 more). The game enters such a map only from the one map whose link
   leads to it, and that map stays loaded in the other slot (040's entrance looks back into 039, and its
   floor is 039's): 1 and that map's number, with the MapLoadRequest position that puts it where it
   joins (the link's position negated: the link's map seen from this one), or 0 when the map asks for
   neighbours itself or no link leads to it. Read from the disc's map files once, then kept. */
int port_map_way_in(int map, int *from, float position[3]);

/* Asks for the player to be put in the map at its place-th place (0: the map's start), as a quick
   load: taken in play, with a message in port_quick_message. 0 when the map or place does not exist. */
int port_teleport_request(int map, int place);

/* Reads --teleport [VBLANK:]MAP[:PLACE] (port/platform/main_host.c) */
void port_teleport_init(void);
/* (the kernel, at each vblank, before the quick saves) asks for the command line's teleport when due */
void port_teleport_vblank(void);

#ifdef __cplusplus
}
#endif

#endif
