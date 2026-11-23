/* Card import (port/platform/card_import.c): King's Field IV's saves from a PS2 emulator's memory card, as PCSX2
   and RetroArch keep one, brought into the save folder (port/include/port/save_files.h).

   A card is a card image (PCSX2's or RetroArch's Mcd001.ps2: 8 MB, with or without the ECC bytes after each page)
   or a PCSX2 folder card (a folder Mcd001.ps2/ holding the card's files as plain files). The game's folder on it,
   BASLUS-20318S, holds the save list (three 0x180-byte records, the game's SaveInfo) and save N in
   BASLUS-20318G0N; a save's data is the save image the PC build keeps as it is, so nothing is converted.

   An import never replaces a save: a card's save goes to the Holyland save of its own number when that is free,
   else to the first free one, and when none is free it is not imported.

   Plain C on the C library and save_files.c (tests/save_files_test.c builds it with that file). A card is a file
   from outside the program: every number read from it is checked before it is used. */
#ifndef PORT_CARD_IMPORT_H
#define PORT_CARD_IMPORT_H

#include <stddef.h>
#include <stdint.h>

#include "port/save_files.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PORT_CARD_OK,         /* read: its whole saves are in PortCardSaves (perhaps none) */
    PORT_CARD_UNREADABLE, /* the file or folder cannot be read */
    PORT_CARD_NOT_CARD,   /* not a PS2 memory card (or a card never formatted) */
    PORT_CARD_NO_GAME,    /* a card without King's Field IV's folder */
    PORT_CARD_DAMAGED,    /* the game's folder is there but its save list is missing or damaged */
} PortCardResult;

/* One save on a card */
typedef struct {
    int number;                             /* the card's save number, 0..PORT_SAVE_COUNT-1 */
    uint8_t record[PORT_SAVE_RECORD_SIZE];  /* what the save list shows of it (SaveInfo) */
    size_t size;                            /* the data's size (its first word) */
    uint8_t data[PORT_SAVE_DATA_MAX];
} PortCardSave;

/* A card's saves (about 400 KB: the caller keeps it off the stack) */
typedef struct {
    int count;   /* whole saves, in saves[0..count-1] */
    int damaged; /* saves the list names whose files are not whole: left out */
    PortCardSave saves[PORT_SAVE_COUNT];
} PortCardSaves;

/* Reads the King's Field IV saves on the card at `path` (an image file or a folder card) into *saves */
PortCardResult port_card_read(const char *path, PortCardSaves *saves);

/* What a save's record says, for a list: the player's level and the play time in whole minutes */
int port_card_save_level(const PortCardSave *save);
int port_card_save_minutes(const PortCardSave *save);

/* The Holyland save that already holds this save's data, or -1 */
int port_card_save_held_in(const PortCardSave *save);
/* The free Holyland save it would go to (its own number when free, else the first free), or -1 when none is free */
int port_card_save_slot(const PortCardSave *save);
/* Writes it to port_card_save_slot. Returns the save written, or -1 (none free, or the write failed:
   port_saves_message says why) */
int port_card_save_import(const PortCardSave *save);

/* kf4 --import-card: every save on the card at `path` imported as above, what happened said on stderr. Returns the
   number of saves imported, or -1 when the card cannot be read */
int port_saves_import_card(const char *path);

#ifdef __cplusplus
}
#endif

#endif
