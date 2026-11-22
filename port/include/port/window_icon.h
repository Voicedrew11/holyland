/* The program's icon on a window: packaging/shared/holyland.bmp, built into the program
   (cmake/embed_file.cmake). X11 desktops take a window's icon only from the window itself; Wayland takes it
   from the desktop entry named by the app id ("holyland"), and Windows from the .exe's icon resource, so
   there it changes nothing. Shared by the game's window and the launcher's. */
#ifndef PORT_WINDOW_ICON_H
#define PORT_WINDOW_ICON_H

#include <SDL3/SDL_video.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Main thread (SDL's video thread). Returns 1 when set; otherwise 0, with why on stderr, and the window
   keeps the desktop's default icon. */
int port_window_icon_set(SDL_Window *window);

#ifdef __cplusplus
}
#endif

#endif
