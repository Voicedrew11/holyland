/* The PC under the game (port/include/port/host_load.h) */
#ifdef _WIN32
#include <windows.h>
#include <pdh.h>
#include <powrprof.h>
#include <psapi.h>
#else
#include <unistd.h>
#endif
#include <stdio.h>

#include "port/host_load.h"
#include "port/platform.h"

#define MEASURE_EVERY_S 0.5

#ifdef _WIN32
/* (winnt's ddk header has it; its layout is CallNtPowerInformation's documented one) */
typedef struct {
    ULONG Number;
    ULONG MaxMhz;
    ULONG CurrentMhz;
    ULONG MhzLimit;
    ULONG MaxIdleState;
    ULONG CurrentIdleState;
} ProcessorPower;

#define MOST_CPUS 256

/* The base and the limit, averaged over the logical processors */
static void processor_power(PortHostLoad *load) {
    static ProcessorPower each[MOST_CPUS];
    SYSTEM_INFO system;
    ULONG count;
    double most = 0, limit = 0;

    GetSystemInfo(&system);
    count = system.dwNumberOfProcessors < MOST_CPUS ? system.dwNumberOfProcessors : MOST_CPUS;
    if (count == 0 ||
        CallNtPowerInformation(ProcessorInformation, NULL, 0, each, (ULONG)(sizeof(each[0]) * count)) != 0) {
        return;
    }
    for (ULONG i = 0; i < count; i++) {
        most += each[i].MaxMhz;
        limit += each[i].MhzLimit;
    }
    load->cpu_most_mhz = most / count;
    load->cpu_limit_mhz = limit / count;
}

/* "% Processor Performance": the speed against the base, over the time since the last collect (Task Manager's) */
static double performance_percent(void) {
    static PDH_HQUERY query;
    static PDH_HCOUNTER counter;
    static int tried;
    PDH_FMT_COUNTERVALUE value;

    if (!tried) {
        tried = 1;
        if (PdhOpenQueryW(NULL, 0, &query) != ERROR_SUCCESS) {
            query = NULL;
            return 0;
        }
        if (PdhAddEnglishCounterW(query, L"\\Processor Information(_Total)\\% Processor Performance", 0, &counter) !=
                ERROR_SUCCESS ||
            PdhCollectQueryData(query) != ERROR_SUCCESS) {
            PdhCloseQuery(query);
            query = NULL;
        }
        return 0; /* (a rate: known from the next collect) */
    }
    if (query == NULL || PdhCollectQueryData(query) != ERROR_SUCCESS ||
        PdhGetFormattedCounterValue(counter, PDH_FMT_DOUBLE, NULL, &value) != ERROR_SUCCESS) {
        return 0;
    }
    return value.doubleValue;
}

static void measure(PortHostLoad *load) {
    PROCESS_MEMORY_COUNTERS_EX memory;
    double percent;

    processor_power(load);
    percent = performance_percent();
    load->cpu_mhz = percent > 0 ? load->cpu_most_mhz * percent / 100.0 : 0;
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&memory, sizeof(memory))) {
        load->memory_mb = (double)memory.PrivateUsage / (1024.0 * 1024.0);
    }
}
#else
/* A number in a file of /sys or /proc, 0 when there is none */
static double read_number(const char *path) {
    FILE *f = fopen(path, "r");
    double value = 0;

    if (f == NULL) {
        return 0;
    }
    if (fscanf(f, "%lf", &value) != 1) {
        value = 0;
    }
    fclose(f);
    return value;
}

static void measure(PortHostLoad *load) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    double sum = 0, most = 0, resident_pages = 0;
    long counted = 0;
    char path[96];
    FILE *statm;

    for (long i = 0; i < cpus && i < 1024; i++) {
        double khz, top;

        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpufreq/scaling_cur_freq", i);
        khz = read_number(path);
        if (khz <= 0) {
            continue;
        }
        snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", i);
        top = read_number(path);
        sum += khz;
        most = top > most ? top : most;
        counted++;
    }
    if (counted > 0) {
        load->cpu_mhz = sum / (double)counted / 1000.0;
        load->cpu_most_mhz = most / 1000.0;
    }
    statm = fopen("/proc/self/statm", "r");
    if (statm != NULL) {
        double size_pages;

        if (fscanf(statm, "%lf %lf", &size_pages, &resident_pages) == 2) {
            load->memory_mb = resident_pages * (double)sysconf(_SC_PAGESIZE) / (1024.0 * 1024.0);
        }
        fclose(statm);
    }
}
#endif

PortHostLoad port_host_load(void) {
    static PortHostLoad last;
    static double measured_at = -1e30;
    double now = port_wall_seconds();

    if (now - measured_at >= MEASURE_EVERY_S) {
        PortHostLoad fresh = {0, 0, 0, 0};

        measure(&fresh);
        last = fresh;
        measured_at = now;
    }
    return last;
}

PortClockCost port_clock_cost(void) {
    enum { READS = 4096, TRIES = 5 };
    static PortClockCost cost;
    static int measured;
    double least_s = 1e30;

    if (measured) {
        return cost;
    }
    measured = 1;
    for (int t = 0; t < TRIES; t++) {
        const uint64_t from = port_ticks();
        uint64_t last = from;

        for (int i = 0; i < READS; i++) {
            last = port_ticks();
        }
        const double s = (double)(last - from) / port_ticks_per_second();
        least_s = s < least_s ? s : least_s;
    }
    cost.read_ns = least_s * 1e9 / READS;
    cost.counter_mhz = port_ticks_per_second() / 1e6;
    return cost;
}
