/* Asking the system to run the game as a game (port/include/port/play_priority.h) */
#include "port/play_priority.h"

#ifdef _WIN32
#include <windows.h>
#include <stdio.h>

/* Both looked up rather than linked: SetProcessInformation is Windows 8's, the power throttling class Windows 10
   1709's (older ones refuse it), and avrt.dll is a DLL of its own */
typedef BOOL(WINAPI *SetInformation)(HANDLE, PROCESS_INFORMATION_CLASS, LPVOID, DWORD);
typedef HANDLE(WINAPI *SetMmThread)(LPCWSTR, LPDWORD);

void port_play_process(void) {
    HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    SetInformation set =
        kernel32 ? (SetInformation)(void (*)(void))GetProcAddress(kernel32, "SetProcessInformation") : NULL;
    PROCESS_POWER_THROTTLING_STATE state;

    if (set == NULL) {
        fprintf(stderr, "[kf4] power throttling: this Windows has none to turn off\n");
        return;
    }
    ZeroMemory(&state, sizeof(state));
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    /* (each flag in the control mask and not in the state mask: that throttling never applied to this process) */
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    state.StateMask = 0;
    if (!set(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof(state))) {
        fprintf(stderr, "[kf4] power throttling: not turned off (error %lu)\n", (unsigned long)GetLastError());
        return;
    }
    fprintf(stderr, "[kf4] power throttling: off for the game\n");
}

void port_play_thread(void) {
    /* (a handful of threads ask, once each: looked up each time; avrt.dll stays loaded for the program's life, as each
       thread's registration lasts as long as the thread) */
    HMODULE avrt = LoadLibraryW(L"avrt.dll");
    SetMmThread set = avrt ? (SetMmThread)(void (*)(void))GetProcAddress(avrt, "AvSetMmThreadCharacteristicsW") : NULL;
    DWORD task_index = 0;

    if (set == NULL || set(L"Games", &task_index) == NULL) {
        /* (a nicety: the thread runs as before) */
        fprintf(stderr, "[kf4] a thread not in Windows's \"Games\" scheduling (error %lu)\n", (unsigned long)GetLastError());
    }
}
#else
void port_play_process(void) {}

void port_play_thread(void) {}
#endif
