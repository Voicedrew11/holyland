/* Save files (port/platform/save_files.c): the game's three saves as plain files on the PC.

   The PS2 game kept its saves on a memory card. On the PC there is no card: game/saves.c stands in for
   the game's card layer (save/save_card.c) and keeps each save here, in the save folder:

     <data folder>/saves/save1.kf4save, save2.kf4save, save3.kf4save

   (--saves DIR names another folder; --saves none: no folder, saving is off, as the tests run). A file
   holds everything about its save: the line the game's save list shows of it (the 0x180-byte record, the
   game's SaveInfo) and the save data Game_WriteSaveImage made, so a save can be copied, backed up or
   deleted on its own.

   The file, every header number little-endian (written byte by byte, never as a C struct):
     0   "KF4SAVE\0"
     8   u32 format version (1)
     12  u32 header size (40): what follows the header is the record, then the data
     16  u32 the record's size (0x180)
     20  u32 the data's size (the save image's first word; at most PORT_SAVE_DATA_MAX)
     24  u32 0
     28  u32 0
     32  u64 FNV-1a hash of the record and the data
     40  the record, then the data, as the game's memory holds them

   A write never harms the save it replaces: the new file is written beside it (saveN.kf4save.tmp) and
   flushed to the disk, the old one becomes saveN.kf4save.bak, then the new one takes its name. A crash
   at any point leaves either save whole; port_saves_open finishes or undoes what a crash cut short.
   A save whose file is damaged is read from its .bak when that is whole (and a write over it then keeps
   that .bak); a deleted save stays deleted, whatever .bak is left. */
#ifndef PORT_SAVE_FILES_H
#define PORT_SAVE_FILES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_SAVE_COUNT 3
#define PORT_SAVE_RECORD_SIZE 0x180u
#define PORT_SAVE_DATA_MAX 0x20000u

typedef enum {
    PORT_SAVE_EMPTY,   /* no file */
    PORT_SAVE_OK,
    PORT_SAVE_DAMAGED, /* a file that is not a whole save (cut short, changed, or not a save file) */
} PortSaveState;

/* Uses `folder` as the save folder, making it (and the folders above it) if it is missing, and mends
   what a crash left there. NULL or "none": no folder. Returns 1 when there is a folder to save in. port/platform/main_host.c
   calls it at start with --saves, or <data folder>/saves; the tests with their own. The name is copied. */
int port_saves_open(const char *folder);

/* 1 when there is a save folder */
int port_saves_available(void);
/* The save folder, or "" */
const char *port_saves_folder(void);

/* What save `index` (0..PORT_SAVE_COUNT-1) is; with PORT_SAVE_OK and `record` not NULL, its record
   (PORT_SAVE_DAMAGED: neither the file nor its backup is whole). */
PortSaveState port_save_record(int index, uint8_t record[PORT_SAVE_RECORD_SIZE]);

/* Reads save `index`: its record and its data (at most `capacity` bytes go to `data`; *size is the
   data's whole size). Returns PORT_SAVE_OK, or why not. */
PortSaveState port_save_read(int index, uint8_t record[PORT_SAVE_RECORD_SIZE], uint8_t *data, size_t capacity,
                             size_t *size);

/* Writes save `index` from the record and `size` bytes of data. Returns 1 when the new save is on the
   disk under its name, 0 when not (the save it replaces, if any, is then as it was). */
int port_save_write(int index, const uint8_t record[PORT_SAVE_RECORD_SIZE], const uint8_t *data, size_t size);

/* The last thing a read or write did, for the log and the overlay */
const char *port_saves_message(void);

#ifdef __cplusplus
}
#endif

#endif
