/* A thread's name (port/include/port/thread_name.h) */
#ifdef _WIN32
#include <windows.h>
#else
#define _GNU_SOURCE
#include <pthread.h>
#endif
#include <stdio.h>
#include <string.h>

#include "port/thread_name.h"

#define NAME_MOST 48 /* with its 0; the profiler's lane names are as long (ZONE_LANE_NAME) */

/* The calling thread's name as port_thread_name last gave it */
static __thread char kept[NAME_MOST];

#ifdef _WIN32
/* SetThreadDescription and GetThreadDescription are in kernel32 from Windows 10 1607 only: looked up, so the
   program still starts on a Windows without them (unnamed threads there) */
typedef HRESULT(WINAPI *SetDescription)(HANDLE, PCWSTR);
typedef HRESULT(WINAPI *GetDescription)(HANDLE, PWSTR *);

static FARPROC kernel32_function(const char *name) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    return kernel32 ? GetProcAddress(kernel32, name) : NULL;
}

static void set_name(const char *name) {
    SetDescription set = (SetDescription)(void (*)(void))kernel32_function("SetThreadDescription");
    WCHAR wide[NAME_MOST];

    if (set == NULL || MultiByteToWideChar(CP_UTF8, 0, name, -1, wide, NAME_MOST) == 0) {
        return;
    }
    /* (a name is a nicety: a failure is let go) */
    (void)set(GetCurrentThread(), wide);
}

static void get_name(char *name, size_t size) {
    GetDescription get = (GetDescription)(void (*)(void))kernel32_function("GetThreadDescription");
    PWSTR wide = NULL;

    name[0] = '\0';
    if (get == NULL || FAILED(get(GetCurrentThread(), &wide)) || wide == NULL) {
        return;
    }
    if (WideCharToMultiByte(CP_UTF8, 0, wide, -1, name, (int)size, NULL, NULL) == 0) {
        name[0] = '\0'; /* (too long for the buffer, or not convertible: no name rather than a cut one) */
    }
    LocalFree(wide);
}
#else
static void set_name(const char *name) {
#ifdef __linux__
    char cut[16];

    snprintf(cut, sizeof(cut), "%s", name);
    (void)pthread_setname_np(pthread_self(), cut);
#elif defined(__APPLE__)
    /* (macOS names the calling thread only, and keeps up to 63 bytes: the whole name) */
    (void)pthread_setname_np(name);
#else
    (void)name;
#endif
}

static void get_name(char *name, size_t size) {
    name[0] = '\0';
#ifdef __linux__
    if (pthread_getname_np(pthread_self(), name, size) != 0) {
        name[0] = '\0';
    }
#elif defined(__APPLE__)
    if (pthread_getname_np(pthread_self(), name, size) != 0) {
        name[0] = '\0';
    }
#else
    (void)size;
#endif
}
#endif

void port_name_thread(const char *name) {
    if (name == NULL) {
        return;
    }
    set_name(name);
}

const char *port_thread_name(void) {
    get_name(kept, sizeof(kept));
    return kept;
}
