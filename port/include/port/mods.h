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

/* Applies the mods in folder (NULL: <data folder>/mods; "none": none). After port_disc_open. */
void port_mods_open(const char *folder);

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
