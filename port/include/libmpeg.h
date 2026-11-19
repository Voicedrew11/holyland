/*
 * port/include/libmpeg.h
 *
 * libmpeg for the PC port (port/sdk/libmpeg.c): the movies' video decoded on the PC, behind the
 * calls the movie player makes.
 */
#ifndef SCE_LIBMPEG_LIBMPEG_H
#define SCE_LIBMPEG_LIBMPEG_H

#include <stdint.h>

typedef struct {
    int width, height, frameCount;
    int64_t pts, dts;
    int64_t flags;
    int64_t pts2nd, dts2nd;
    int64_t flags2nd;
    void *sys;
} sceMpeg;

/* what a callback is handed: each record starts with the type (sceMpegCbType) */
typedef struct {
    int type;
} sceMpegCbData;

/* a stream callback's (type 6): one PES packet of the stream, still in the read ring */
typedef struct {
    int type;
    unsigned char *header;
    unsigned char *data;
    unsigned int len;
    int64_t pts, dts;
} sceMpegCbDataStr;

/* the time stamp callback's (type 5): it fills in the times of the picture being decoded */
typedef struct {
    int type;
    int64_t pts, dts;
} sceMpegCbDataTimeStamp;

typedef int (*sceMpegCallback)(sceMpeg *mp, void *cbdata, void *anyData);

void sceMpegInit(void);
int sceMpegCreate(sceMpeg *mp, void *work_area, int size);
int sceMpegDelete(sceMpeg *mp);
void sceMpegReset(sceMpeg *mp);
sceMpegCallback sceMpegAddCallback(sceMpeg *mp, int type, sceMpegCallback func, void *data);
int sceMpegAddStrCallback(sceMpeg *mp, int type, int ch, sceMpegCallback func, void *data);
int sceMpegDemuxPssRing(sceMpeg *mp, void *buf, int size, void *ring, int ring_size);
int sceMpegGetPicture(sceMpeg *mp, void *rgb32, int mbcount);
int sceMpegIsEnd(sceMpeg *mp);
int sceMpegIsRefBuffEmpty(sceMpeg *mp);
void sceMpegSetDecodeMode(sceMpeg *mp, int ni, int np, int nb);

/* The disc movie's 48 kHz stereo PCM, in SPU2 input order. The caller owns the returned buffer. */
uint8_t *port_mpeg_load_pcm(uint32_t lsn, uint32_t file_size, uint32_t *pcm_size);

#endif /* SCE_LIBMPEG_LIBMPEG_H */
