/* The program's icon on a window (port/include/port/window_icon.h). */
#include "port/window_icon.h"

#include <stdio.h>

#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_surface.h>

/* packaging/shared/holyland.bmp, made into an array by cmake/embed_file.cmake */
extern const unsigned char port_window_icon_bmp[];
extern const size_t port_window_icon_bmp_size;

int port_window_icon_set(SDL_Window *window) {
    SDL_IOStream *bmp;
    SDL_Surface *icon;
    bool set;

    bmp = SDL_IOFromConstMem(port_window_icon_bmp, port_window_icon_bmp_size);
    if (bmp == NULL) {
        fprintf(stderr, "[icon] %s\n", SDL_GetError());
        return 0;
    }
    icon = SDL_LoadBMP_IO(bmp, true); /* (closes bmp, read or not) */
    if (icon == NULL) {
        fprintf(stderr, "[icon] the built-in icon: %s\n", SDL_GetError());
        return 0;
    }
    set = SDL_SetWindowIcon(window, icon);
    if (!set) {
        fprintf(stderr, "[icon] not set on the window: %s\n", SDL_GetError());
    }
    SDL_DestroySurface(icon);
    return set ? 1 : 0;
}
