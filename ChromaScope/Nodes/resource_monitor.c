/* resource_monitor.c — Monitor de recursos del sistema usando /proc.
 *
 * Lee información de uso de memoria y CPU directamente del sistema de archivos
 * virtual /proc del kernel Linux. Esto funciona sin bibliotecas externas
 * porque el kernel expone el estado interno del sistema como archivos de texto.
 *
 * Archivos consultados:
 *   /proc/meminfo  — estadísticas de memoria RAM (MemTotal, MemAvailable, etc.)
 *   /proc/loadavg  — promedios de carga del CPU en los últimos 1, 5 y 15 min
 *
 * Cada worker llama a print_resource_snapshot() después de descifrar su franja,
 * lo que nos permite observar el consumo de recursos en cada nodo del clúster.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include <stdio.h>    /* FILE, fopen, fclose, fgets, printf: I/O de archivos   */
#include <stdlib.h>   /* (incluido por compatibilidad con posibles extensiones) */
#include <string.h>   /* strncmp: comparar prefijos de líneas de /proc/meminfo  */

#include "resource_monitor.h"   /* declaración pública de print_resource_snapshot() */

/* ── Funciones privadas de este módulo ──────────────────────────────────── */
static void print_memory_info(int rank);
static void print_cpu_info(int rank);

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * print_resource_snapshot() — imprime un resumen de memoria y CPU para el nodo.
 *
 * Se llama con el rank MPI del worker para identificar de qué nodo provienen
 * los datos. Agrupa las dos lecturas de /proc bajo un encabezado común.
 */
void print_resource_snapshot(int rank) {
    printf("[Rank %d] Monitor de recursos:\n", rank);  /* encabezado con rank  */
    print_memory_info(rank);   /* leer y mostrar MemTotal y MemAvailable        */
    print_cpu_info(rank);      /* leer y mostrar el load average del CPU        */
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * print_memory_info() — lee /proc/meminfo y muestra las dos primeras líneas
 * relevantes: MemTotal (RAM física instalada) y MemAvailable (RAM libre).
 *
 * /proc/meminfo tiene este formato en cada línea:
 *   MemTotal:       7812744 kB
 *   MemFree:         123456 kB
 *   MemAvailable:   5432100 kB
 *   ...
 *
 * Solo nos interesan MemTotal y MemAvailable para evaluar la presión de memoria
 * durante el procesamiento distribuido de imágenes.
 */
static void print_memory_info(int rank) {

    /* Abrir el archivo virtual del kernel para leer estadísticas de memoria.   */
    FILE *file = fopen("/proc/meminfo", "r");
    if (file == NULL) {
        printf("[Rank %d] No se pudo leer /proc/meminfo\n", rank);
        return;
    }

    char line[256];   /* buffer para una línea de texto de /proc/meminfo        */
    int printed = 0;  /* contador de líneas útiles ya impresas                  */

    /* Leer línea a línea hasta encontrar las dos entradas de interés o EOF.    */
    while (fgets(line, sizeof(line), file) != NULL && printed < 2) {

        /* strncmp compara solo los primeros N caracteres del prefijo.          */
        if (strncmp(line, "MemTotal:", 9) == 0 ||       /* RAM total instalada  */
            strncmp(line, "MemAvailable:", 13) == 0) {  /* RAM disponible ahora */
            printf("[Rank %d]   %s", rank, line);   /* imprimir con el rank    */
            printed++;                               /* contar la línea impresa  */
        }
    }

    fclose(file);   /* cerrar el handle de /proc/meminfo                        */
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * print_cpu_info() — lee /proc/loadavg y muestra el promedio de carga del CPU.
 *
 * /proc/loadavg tiene una sola línea con el formato:
 *   0.25 0.15 0.08 1/253 12345
 *   ^^^^ ^^^^ ^^^^
 *   1min 5min 15min  (número de procesos en cola / total)  (PID último proceso)
 *
 * Un load average de 1.0 en un núcleo significa uso completo de ese núcleo.
 * En la RPi 4 (4 núcleos), un load de 4.0 indica todos los núcleos ocupados.
 * Valores bajos durante el procesamiento MPI indican buena distribución de carga.
 */
static void print_cpu_info(int rank) {

    /* Abrir el archivo virtual del kernel para leer el promedio de carga.      */
    FILE *file = fopen("/proc/loadavg", "r");
    if (file == NULL) {
        printf("[Rank %d] No se pudo leer /proc/loadavg\n", rank);
        return;
    }

    char line[256];   /* buffer para la única línea de /proc/loadavg            */

    /* Leer la primera (y única) línea del archivo.                             */
    if (fgets(line, sizeof(line), file) != NULL) {
        printf("[Rank %d]   Load average: %s", rank, line);   /* mostrar datos  */
    }

    fclose(file);   /* cerrar el handle de /proc/loadavg                        */
}
