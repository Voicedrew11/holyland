/* The IOP side for the PC port (port/include/sifdev.h, port/include/sifrpc.h): file calls on cdrom0:
   and host0:, the IOP's modules and heap, SIF DMA and RPC.

   There is no IOP. Its modules "load" (each is checked to be on the disc), its heap hands out IOP
   addresses that nothing reads back, a SIF transfer is done as soon as it is set, and an RPC is answered
   before sceSifCallRpc returns. The one RPC server the game binds is FromSoftware's sound driver,
   FSIOPSND (server 0x1010, port/sdk/fssnd.c): until sound (step 4) it takes every command and answers
   as the driver would when all goes well, without making a sound. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port/host.h"
#include "port/platform.h"
#include "sifdev.h"
#include "sifrpc.h"

/* ---- Files ------------------------------------------------------------------------------------ */
#define MAX_FILES 16

typedef struct {
    int used;
    int on_disc;      /* cdrom0: (read only), else host0: */
    FILE *host;
    uint32_t lsn;     /* cdrom0: the file's first sector, size and read position */
    uint32_t size;
    uint32_t position;
} OpenFile;

static OpenFile open_files[MAX_FILES];

/* The part after the device ("cdrom0:\DATA\X;1" gives "\DATA\X;1"), and whether it is the disc;
   NULL for a device the PC does not have. */
static const char *split_device(const char *name, int *on_disc) {
    const char *colon = strchr(name, ':');
    size_t length;

    if (colon == NULL) {
        return NULL;
    }
    length = (size_t)(colon - name);
    if ((length == 6 && strncmp(name, "cdrom0", 6) == 0) || (length == 5 && strncmp(name, "cdrom", 5) == 0)) {
        *on_disc = 1;
        return colon + 1;
    }
    if ((length == 5 && strncmp(name, "host0", 5) == 0) || (length == 4 && strncmp(name, "host", 4) == 0)) {
        *on_disc = 0;
        return colon + 1;
    }
    return NULL;
}

int sceOpen(const char *name, int flags, ...) {
    const char *path;
    int on_disc = 0;
    int fd;

    path = split_device(name, &on_disc);
    if (path == NULL) {
        port_event("sceOpen: no device for %s", name);
        return -1;
    }
    for (fd = 0; fd < MAX_FILES && open_files[fd].used; fd++) {
    }
    if (fd == MAX_FILES) {
        return -1;
    }
    if (on_disc) {
        OpenFile *f = &open_files[fd];

        if ((flags & 3) != SCE_RDONLY || !port_disc_find(path, &f->lsn, &f->size)) {
            port_event("sceOpen: cannot open %s", name);
            return -1;
        }
        f->used = 1;
        f->on_disc = 1;
        f->position = 0;
        port_event("open %s", name);
    } else {
        /* host0: is a folder on the PC (--host-dir); the game only writes its debug dumps there */
        char host_path[1024];
        const char *mode;
        FILE *file;

        while (*path == '/' || *path == '\\') {
            path++;
        }
        snprintf(host_path, sizeof(host_path), "%s/%s", port_options.host_dir ? port_options.host_dir : ".", path);
        mode = (flags & 3) == SCE_RDONLY ? "rb" : (flags & SCE_TRUNC) || (flags & SCE_CREAT) ? "w+b" : "r+b";
        file = fopen(host_path, mode);
        if (file == NULL) {
            return -1;
        }
        open_files[fd].used = 1;
        open_files[fd].on_disc = 0;
        open_files[fd].host = file;
        port_event("open host0:%s", path);
    }
    return fd;
}

static OpenFile *file_of(int fd) {
    return fd >= 0 && fd < MAX_FILES && open_files[fd].used ? &open_files[fd] : NULL;
}

int sceClose(int fd) {
    OpenFile *f = file_of(fd);

    if (f == NULL) {
        return -1;
    }
    if (f->host != NULL) {
        fclose(f->host);
    }
    memset(f, 0, sizeof(*f));
    return 0;
}

int sceRead(int fd, void *buffer, int size) {
    OpenFile *f = file_of(fd);
    unsigned char sector[DISC_SECTOR_SIZE];
    unsigned char *out = buffer;
    int done = 0;

    if (f == NULL || size < 0) {
        return -1;
    }
    if (!f->on_disc) {
        return (int)fread(buffer, 1, (size_t)size, f->host);
    }
    if ((uint32_t)size > f->size - f->position) {
        size = (int)(f->size - f->position);
    }
    while (done < size) {
        uint32_t offset = f->position % DISC_SECTOR_SIZE;
        uint32_t chunk = DISC_SECTOR_SIZE - offset;

        if (chunk > (uint32_t)(size - done)) {
            chunk = (uint32_t)(size - done);
        }
        if (!port_disc_read(f->lsn + f->position / DISC_SECTOR_SIZE, 1, sector)) {
            break;
        }
        memcpy(out + done, sector + offset, chunk);
        done += (int)chunk;
        f->position += chunk;
    }
    return done;
}

int sceWrite(int fd, const void *buffer, int size) {
    OpenFile *f = file_of(fd);

    if (f == NULL || f->on_disc || size < 0) {
        return -1;
    }
    return (int)fwrite(buffer, 1, (size_t)size, f->host);
}

int sceLseek(int fd, int offset, int whence) {
    OpenFile *f = file_of(fd);
    int64_t position;

    if (f == NULL) {
        return -1;
    }
    if (!f->on_disc) {
        if (fseek(f->host, offset, whence == SCE_SEEK_END ? SEEK_END : whence == SCE_SEEK_CUR ? SEEK_CUR : SEEK_SET) != 0) {
            return -1;
        }
        return (int)ftell(f->host);
    }
    position = whence == SCE_SEEK_END ? (int64_t)f->size + offset
             : whence == SCE_SEEK_CUR ? (int64_t)f->position + offset
                                      : (int64_t)offset;
    if (position < 0 || position > (int64_t)f->size) {
        return -1;
    }
    f->position = (uint32_t)position;
    return (int)position;
}

int sceFsReset(void) {
    return 0;
}

/* ---- The IOP and its modules ------------------------------------------------------------------ */
int sceSifRebootIop(const char *image) {
    int on_disc = 0;
    const char *path = split_device(image, &on_disc);

    if (path == NULL || !on_disc || !port_disc_find(path, NULL, NULL)) {
        PORT_FATAL("the IOP's image %s is not on the disc", image);
    }
    return 1;
}

int sceSifSyncIop(void) {
    return 1;
}

static int next_module_id = 1;

int sceSifLoadModule(const char *path, int arg_length, const char *args) {
    int on_disc = 0;
    const char *name = split_device(path, &on_disc);

    (void)arg_length;
    (void)args;
    if (name == NULL || !on_disc || !port_disc_find(name, NULL, NULL)) {
        port_event("sceSifLoadModule: %s is not on the disc", path);
        return -1;
    }
    port_event("IOP module %s", path);
    return next_module_id++;
}

/* ---- The IOP heap ----------------------------------------------------------------------------
   IOP addresses, first fit, freed as the game frees them. The IOP's 2 MB hold the modules too; the PC
   gives the heap the room it had (about 1.5 MB), so that a game that runs out of it runs out here
   as well. */
#define IOP_HEAP_START 0x00080000u
#define IOP_HEAP_END 0x00200000u
#define MAX_IOP_BLOCKS 512

typedef struct {
    uint32_t address;
    uint32_t size;
} IopBlock;

static IopBlock iop_blocks[MAX_IOP_BLOCKS]; /* the blocks in use, by address */
static int iop_block_count;

int sceSifInitIopHeap(void) {
    return 0;
}

int sceSifAllocIopHeap(int size) {
    uint32_t want = ((uint32_t)size + 0xFF) & ~0xFFu;
    uint32_t start = IOP_HEAP_START;
    int i;

    if (size <= 0 || iop_block_count == MAX_IOP_BLOCKS) {
        return 0;
    }
    for (i = 0; i <= iop_block_count; i++) {
        uint32_t end = i < iop_block_count ? iop_blocks[i].address : IOP_HEAP_END;

        if (end - start >= want) {
            memmove(&iop_blocks[i + 1], &iop_blocks[i], (size_t)(iop_block_count - i) * sizeof(IopBlock));
            iop_blocks[i].address = start;
            iop_blocks[i].size = want;
            iop_block_count++;
            return (int)start;
        }
        if (i < iop_block_count) {
            start = iop_blocks[i].address + iop_blocks[i].size;
        }
    }
    port_event("sceSifAllocIopHeap: no room for 0x%X bytes", (unsigned)size);
    return 0;
}

int sceSifFreeIopHeap(int iop_address) {
    int i;

    for (i = 0; i < iop_block_count; i++) {
        if (iop_blocks[i].address == (uint32_t)iop_address) {
            memmove(&iop_blocks[i], &iop_blocks[i + 1], (size_t)(iop_block_count - i - 1) * sizeof(IopBlock));
            iop_block_count--;
            return 0;
        }
    }
    return -1;
}

int sceSifLoadIopHeap(const char *path, int iop_address) {
    int on_disc = 0;
    const char *name = split_device(path, &on_disc);

    (void)iop_address;
    if (name == NULL || !on_disc || !port_disc_find(name, NULL, NULL)) {
        port_event("sceSifLoadIopHeap: %s is not on the disc", path);
        return -1;
    }
    port_event("IOP load %s", path);
    return 0;
}

/* ---- SIF DMA: done as soon as it is set ------------------------------------------------------- */
static unsigned int next_dma_id = 1;

unsigned int sceSifSetDma(sceSifDmaData *transfers, int count) {
    (void)transfers;
    (void)count;
    if (++next_dma_id == 0) {
        next_dma_id = 1;
    }
    return next_dma_id;
}

int sceSifDmaStat(unsigned int id) {
    (void)id;
    return -1;
}

/* ---- RPC -------------------------------------------------------------------------------------- */
int port_fssnd_call(unsigned int function, void *send_buffer, int send_size, void *receive_buffer,
                    int receive_size);

typedef struct {
    unsigned int id;
    int (*call)(unsigned int function, void *send_buffer, int send_size, void *receive_buffer,
                int receive_size);
} RpcServer;

static RpcServer rpc_servers[] = {
    {0x1010, port_fssnd_call},
};

void sceSifInitRpc(int mode) {
    (void)mode;
}

int sceSifBindRpc(sceSifClientData *cd, unsigned int server_id, int mode) {
    size_t i;

    (void)mode;
    memset(cd, 0, sizeof(*cd));
    cd->command = server_id;
    for (i = 0; i < sizeof(rpc_servers) / sizeof(rpc_servers[0]); i++) {
        if (rpc_servers[i].id == server_id) {
            cd->server = &rpc_servers[i];
            return 0;
        }
    }
    PORT_FATAL("the game binds RPC server 0x%X, which the PC does not have", server_id);
}

int sceSifCallRpc(sceSifClientData *cd, unsigned int function, unsigned int mode, void *send_buffer,
                  int send_size, void *receive_buffer, int receive_size, sceSifEndFunc end_function,
                  void *end_param) {
    RpcServer *server = cd->server;

    (void)mode;
    if (server == NULL) {
        PORT_FATAL("sceSifCallRpc on a client that is not bound");
    }
    server->call(function, send_buffer, send_size, receive_buffer, receive_size);
    if (end_function != NULL) {
        end_function(end_param);
    }
    return 0;
}

int sceSifCheckStatRpc(sceSifClientData *cd) {
    (void)cd;
    return 0;
}
