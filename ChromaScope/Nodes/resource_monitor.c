#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "resource_monitor.h"

static void print_memory_info(int rank);
static void print_cpu_info(int rank);

void print_resource_snapshot(int rank) {
    printf("[Rank %d] Monitor de recursos:\n", rank);
    print_memory_info(rank);
    print_cpu_info(rank);
}

static void print_memory_info(int rank) {
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
        printf("[Rank %d] No se pudo leer /proc/meminfo\n", rank);
        return;
    }

    char line[256];
    int printed = 0;

    while (fgets(line, sizeof(line), file) != NULL && printed < 2) {
        if (strncmp(line, "MemTotal:", 9) == 0 ||
            strncmp(line, "MemAvailable:", 13) == 0) {
            printf("[Rank %d]   %s", rank, line);
            printed++;
        }
    }

    fclose(file);
}

static void print_cpu_info(int rank) {
    FILE *file = fopen("/proc/loadavg", "r");
    if (file == NULL) {
        printf("[Rank %d] No se pudo leer /proc/loadavg\n", rank);
        return;
    }

    char line[256];

    if (fgets(line, sizeof(line), file) != NULL) {
        printf("[Rank %d]   Load average: %s", rank, line);
    }

    fclose(file);
}