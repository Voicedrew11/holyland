/* The release this build is (docs/port/LAUNCHER.md, "Versions"): "0.2.0" in a launcher's build, from the
   release's tag through KF4_VERSION at configure; "" in a developer's build, which is never told about
   updates. port_build_rev is the git commit (or the version, in a release's source, which has no git). */
#ifndef PORT_VERSION_H
#define PORT_VERSION_H

#ifdef __cplusplus
extern "C" {
#endif

extern const char port_version[];
/* 1 when the port's code was compiled with the compiler's optimizations (-O1 and up), 0 when not (a Debug build, an
   IDE's default): such a build runs several times slower and stutters (a player's on Windows, 2026-11-23). The log's
   first line and the profiler (Shift+P) say so. */
extern const int port_build_optimized;

#ifdef __cplusplus
}
#endif

#endif
