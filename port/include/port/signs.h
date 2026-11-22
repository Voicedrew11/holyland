/* Finer sign pictures (port/platform/signs.c): the game shows a sign, gravestone or inscription as a picture,
   msg/mNNNN.tm2 (512 x 256, ui/splash.c). A mod can give a finer one beside it, signs/mNNNN.png, the same picture
   2, 3 or 4 times as wide and as tall (1024 x 512 to 2048 x 1024, 8-bit RGB or RGBA, any PNG colour type but
   interlaced); the scene renderer draws it in place of the game's, which the GS and the other renderers still draw.

   The picture the game loaded is known to the renderer by its texels: the game tells the port which picture it
   loaded (port_sign_loaded) and the port keeps a fingerprint of its indices; the renderer, decoding a 512 x 256
   palette texture, asks for a finer one by the same fingerprint (port_sign_finer). */
#ifndef PORT_SIGNS_H
#define PORT_SIGNS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* (game code, ui/splash.c) picture `id` was read: the file as the disc or a mod has it */
void port_sign_loaded(int id, const void *file, size_t size);

/* The fingerprint of a 512 x 256 picture's indices, one byte a texel, rows top down (the renderer's decoded texture,
   or the file's unpacked) */
uint64_t port_sign_fingerprint(const uint8_t *indices, size_t count);

/* (renderer) the finer picture of the sign with that fingerprint: 1 with *rgba (scale*512 x scale*256 colours, rows
   top down, the GS's alpha: 0x80 opaque; kept until the program ends) and *scale, 0 none */
int port_sign_finer(uint64_t fingerprint, const uint32_t **rgba, uint32_t *scale);

#ifdef __cplusplus
}
#endif

#endif
