/* Mods (port/platform/mods.c): folders of replacement game files and text, read at start.

   Each folder in the mods folder (<data folder>/mods, or --mods DIR; --mods none: no mods) is one mod,
   applied in name order, so a later mod wins where two change the same thing:

     <mod>/files/<member>   a whole member of KF4.DAT, by its name in the archive ("font.txc",
                            "param/item_nam.prm", "msg/m0010.tm2")
     <mod>/text.json        text in place of the game's own:
                              { "messages": { "<id>": "text", ... },   font.txc, by message id
                                "items":    { "<number>": "name", ... }, param/item_nam.prm
                                "spells":   { "<record>": "name", ... } } param/magic.prm
                            Text is UTF-8 in the US font: printable ASCII, "\n" for a new line, and
                            {num3} (a number argument, 3 digits) as in the game's own messages.

   The new files are kept in memory and the disc reads them in place of the disc's (port_disc_replace_member).
   A mod folder whose name starts with "." or "_" is skipped (a quick way to turn one off). */
#ifndef PORT_MODS_H
#define PORT_MODS_H

#include <stddef.h>
#include <stdint.h>

/* ---- The mod list ------------------------------------------------------------------------------------

   A mod may describe itself in <mod>/mod.json (modding/include/holyland_mod.h has the format): its name, version,
   author, description, the API it needs, its plugin for each system, its settings and its keys. A mod without one
   is a data mod named by its folder.

   The player turns each mod on or off on the player menu's Mods page (settings.json's "mod_list": {"<mod>": {"on":
   true, "settings": {"<key>": 1}}}); a mod is on by default unless it has a plugin. On and off take effect at the
   next start: the files and text are applied, and plugins loaded, once. */

#define PORT_MOD_SETTINGS 16
#define PORT_MOD_KEYS 8
#define PORT_MOD_CHOICES 8

typedef enum { PORT_MOD_TOGGLE, PORT_MOD_CHOICE, PORT_MOD_NUMBER } PortModSettingType;

typedef struct {
    char key[48];
    char label[96];
    char help[320];
    PortModSettingType type;
    int value, fallback;      /* now, and mod.json's "default" */
    int min, max, step;       /* a number's */
    int choice_count;         /* a choice's */
    char choices[PORT_MOD_CHOICES][48];
} PortModSetting;

typedef struct {
    char key[48];
    char label[96];
    int action;               /* its PortAction (ACTION_MOD_FIRST ...), -1 none left */
} PortModKey;

typedef struct {
    char id[64];              /* the folder's name */
    char folder[1024];        /* its path */
    char name[128];           /* mod.json's "name", else the id */
    char version[32];
    char author[128];
    char description[640];
    char holyland[32];        /* the Holyland it was made for ("0.1.1"), "" any */
    int api;                  /* the API major it needs, 0 none (a data mod) */
    char plugin[1100];        /* its plugin for this system, "" none */
    int has_plugin;           /* it names a plugin for some system */
    int on;                   /* the player's choice (or the default) */
    int applied;              /* on at this start: its files and text applied, its plugin loaded if any */
    int setting_count;
    PortModSetting settings[PORT_MOD_SETTINGS];
    int key_count;
    PortModKey keys[PORT_MOD_KEYS];
} PortMod;

/* Reads the mods folder's mod.json files (folder as port_mods_open's) and gives the mods' keys their actions. Before
   port_settings_load, which then reads the player's choices (port_mods_settings_read) and key bindings. */
void port_mods_scan(const char *folder);
int port_mods_count(void);
PortMod *port_mods_get(int index);
PortMod *port_mods_by_id(const char *id);

/* (port/overlay/settings.c) settings.json's "mod_list" read and written */
void port_mods_settings_read(const void *mod_list);
void port_mods_settings_write(void *mod_list);

/* Applies the mods in folder (NULL: <data folder>/mods; "none": none) that are on: their files and text. After
   port_disc_open. (Scans first if port_mods_scan was not called.) */
void port_mods_open(const char *folder);

/* The path of `file` in the last applied mod that has it ("journal.json"), NULL when none has: for a mod's own
   data that is not a game file. The path is good until the next call. */
const char *port_mods_find(const char *file);

/* The path of `file` for a game file `member`'s mod: in the last applied mod that has files/<member>, when one has
   (NULL if that mod has no `file`), else as port_mods_find. A finer sign picture is the one made with the game's
   picture in use: a mod's signs/m0034.png with its own files/msg/m0034.tm2, or with the disc's when it has none. */
const char *port_mods_find_with(const char *file, const char *member);

/* 1 when the mod folder of that name ("kf4-retranslation") was applied (not skipped, not missing) */
int port_mods_active(const char *name);

/* ---- The parts, for tests ---------------------------------------------------------------------------- */

/* Encodes UTF-8 text in the game's text code (16-bit: 0x08xx a Latin character, 0x8000 a new line,
   0x2n00 {numN}), up to max codes, with no end mark; returns the codes written. Characters the US font lacks
   become '?', a few typographic ones their ASCII look-alikes ('...' for an ellipsis). *unknown, when not
   NULL, counts the characters that became '?'. */
int mods_encode_text(const char *text, uint16_t *out, int max, int *unknown);

/* font.txc with messages[id] (NULL: the original's) for ids below count; a new allocation in *out_size bytes,
   or NULL when the original is not a message table. */
unsigned char *mods_build_messages(const unsigned char *original, size_t size, char *const *messages, int count,
                                   size_t *out_size);

/* Writes names into fixed-size records: names[n] (NULL: unchanged) into record n's first name_words 16-bit
   words, an end mark included (so a name keeps at most name_words - 1 characters; longer ones are cut and
   counted in *cut). */
void mods_put_names(unsigned char *records, size_t size, size_t record_size, int name_words, char *const *names,
                    int count, int *cut);

#endif
