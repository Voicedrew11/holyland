/* FSIOPSND, FromSoftware's sound driver on the IOP (RPC server 0x1010), as a host server that makes no
   sound (port/sdk/sif.c binds it). Until sound (step 4) it takes every command and answers as the
   driver does when all goes well, so the game's waits on the driver end.

   The answer is the first word of the receive buffer (SoundDriver_Remote returns it). Most commands
   answer 0, "done"; the ones below answer what the game waits for or keeps:
     0x10   the IOP address of the driver's DMA source buffer
     0xA0   the IOP address of the driver's work buffer
     0x80   1: the upload to sound memory has finished
     0x90   the size sent (args[4]): the whole upload went
     0x3090 0: the stream is not playing
     0x30B0 1: the stream is ready
   and the stream commands 0x3000|s and 0x30A0|s, which send a file name, answer 0 (opened). */
#include <string.h>

#include "port/host.h"
#include "port/platform.h"

/* IOP addresses in the driver's own memory, below the heap sif.c hands out */
#define FSSND_DMA_SOURCE 0x000F0000
#define FSSND_WORK_BUFFER 0x000F8000

int port_fssnd_call(unsigned int function, void *send_buffer, int send_size, void *receive_buffer,
                    int receive_size) {
    const int32_t *args = (const int32_t *)send_buffer + 1; /* after the record's first word */
    int32_t answer = 0;

    (void)receive_size;
    if (function & 0x80000) {
        /* the record goes out and the driver's status comes back into the game's buffer: none yet */
        return 0;
    }
    if (!(function & 0x40000) && send_buffer != NULL && send_size >= 0x18) {
        switch (function) {
        case 0x10:
            answer = FSSND_DMA_SOURCE;
            break;
        case 0xA0:
            answer = FSSND_WORK_BUFFER;
            break;
        case 0x80:
            answer = 1;
            break;
        case 0x90:
            answer = args[4];
            break;
        case 0x30B0:
            answer = 1;
            break;
        default:
            break;
        }
    }
    /* a word even when the game asks for one byte: it reads the whole word back */
    if (receive_buffer != NULL) {
        memcpy(receive_buffer, &answer, sizeof(answer));
    }
    return 0;
}
