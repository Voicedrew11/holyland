/* A thread's name, given by the thread itself, as the frame profiler's lanes (Shift+P), debuggers and the system's
   tools show it: on Linux the thread's comm, on Windows its description (Windows 10 1607 and later).

   port/platform/thread_name.c, in the profile library (kf4_profile) with the lanes that read it. Each call is about
   the calling thread only. */
#ifndef PORT_THREAD_NAME_H
#define PORT_THREAD_NAME_H

#ifdef __cplusplus
extern "C" {
#endif

/* Names the calling thread. Linux keeps 15 characters, so names are kept that short: "kf4-pc-watch". */
void port_name_thread(const char *name);
/* The calling thread's name: the one it gave itself, else the system's (one another library gave it, or on Linux
   the program's for the main thread); "" if it has none. The text stays the calling thread's until it names
   itself again. */
const char *port_thread_name(void);

#ifdef __cplusplus
}
#endif

#endif /* PORT_THREAD_NAME_H */
