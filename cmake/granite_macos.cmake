# Granite on macOS (docs/port/MACOS.md): what its pinned version needs to build there. Made with the other edits
# (cmake/granite_edits.cmake, after granite_dynamic_state.cmake, whose kf4_granite_replace it uses), on every system,
# so a release's source built on a Mac has it; the code it adds is for __APPLE__ alone, so elsewhere Granite compiles
# as before.
#  - util/timer.cpp: macOS has no clock_nanosleep; sleep_until_nsecs sleeps the time left on the same monotonic clock
#    (get_current_time_nsecs) with nanosleep instead, again after an early wake.
function(kf4_granite_macos granite)
    kf4_granite_replace(${granite}/util/timer.cpp
        "\tint ret;\n\twhile ((ret = clock_nanosleep(timebase, TIMER_ABSTIME, &ts, nullptr)) == EINTR) {}\n"
        "#ifdef __APPLE__ // (holyland: macOS has no clock_nanosleep)\n\t(void)timebase;\n\t(void)ts;\n\tfor (int64_t left; (left = timepoint - get_current_time_nsecs()) > 0;)\n\t{\n\t\tstruct timespec rel = { time_t(left / 1000000000ll), long(left % 1000000000ll) };\n\t\tnanosleep(&rel, nullptr);\n\t}\n#else\n\tint ret;\n\twhile ((ret = clock_nanosleep(timebase, TIMER_ABSTIME, &ts, nullptr)) == EINTR) {}\n#endif\n")
endfunction()
