#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../Server/mpi_protocol.h"
#include "worker.h"
#include "resource_monitor.h"

static void calculate_fake_hsv_histogram(const uint8_t *rgb_data,
                                         int width,
                                         int rows,
                                         int channels,
                                         uint32_t *histogram,
                                         int bins);

static int rgb_to_fake_hue_bin(uint8_t r, uint8_t g, uint8_t b);

void run_worker(int rank) {
    StripMetadata metadata;

    printf("[Rank %d] Worker iniciado, esperando metadata...\n", rank);

    MPI_Recv(&metadata,
             sizeof(StripMetadata),
             MPI_BYTE,
             MASTER_RANK,
             TAG_METADATA,
             MPI_COMM_WORLD,
             MPI_STATUS_IGNORE);

    printf("[Rank %d] Metadata recibida:\n", rank);
    printf("[Rank %d]   Imagen total: %dx%d\n", rank, metadata.width, metadata.height);
    printf("[Rank %d]   Franja: start_row=%d, rows=%d\n",
           rank,
           metadata.start_row,
           metadata.rows);
    printf("[Rank %d]   Bytes esperados: %d\n", rank, metadata.strip_bytes);

    uint8_t *strip_data = malloc((size_t)metadata.strip_bytes);
    if (strip_data == NULL) {
        fprintf(stderr, "[Rank %d] Error: no se pudo reservar memoria para la franja.\n", rank);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    MPI_Recv(strip_data,
             metadata.strip_bytes,
             MPI_UNSIGNED_CHAR,
             MASTER_RANK,
             TAG_IMAGE_STRIP,
             MPI_COMM_WORLD,
             MPI_STATUS_IGNORE);

    printf("[Rank %d] Franja recibida correctamente.\n", rank);

    /*
     * En fase 1 todavía no hay ChaCha20 real.
     * Aquí iría después:
     *
     * chacha20_encrypt_buffer(...)
     * o
     * chacha20_decrypt_buffer(...)
     *
     * De momento asumimos que la franja ya está en RGB plano.
     */

    print_resource_snapshot(rank);

    uint32_t partial_histogram[HIST_BINS];
    memset(partial_histogram, 0, sizeof(partial_histogram));

    calculate_fake_hsv_histogram(strip_data,
                                 metadata.width,
                                 metadata.rows,
                                 metadata.channels,
                                 partial_histogram,
                                 metadata.hist_bins);

    printf("[Rank %d] Histograma parcial calculado.\n", rank);

    uint32_t global_histogram[HIST_BINS];
    memset(global_histogram, 0, sizeof(global_histogram));

    MPI_Reduce(partial_histogram,
            global_histogram,
            metadata.hist_bins,
            MPI_UINT32_T,
            MPI_SUM,
            MASTER_RANK,
            MPI_COMM_WORLD);

    printf("[Rank %d] Histograma parcial enviado mediante MPI_Reduce.\n", rank);

    free(strip_data);
}

static void calculate_fake_hsv_histogram(const uint8_t *rgb_data,
                                         int width,
                                         int rows,
                                         int channels,
                                         uint32_t *histogram,
                                         int bins) {
    int total_pixels = width * rows;

    for (int i = 0; i < total_pixels; i++) {
        int index = i * channels;

        uint8_t r = rgb_data[index + 0];
        uint8_t g = rgb_data[index + 1];
        uint8_t b = rgb_data[index + 2];

        int bin = rgb_to_fake_hue_bin(r, g, b);

        if (bin >= 0 && bin < bins) {
            histogram[bin]++;
        }
    }
}

static int rgb_to_fake_hue_bin(uint8_t r, uint8_t g, uint8_t b) {
    /*
     * Fase 1:
     * Esta función NO es HSV real todavía.
     * Es una aproximación temporal para validar el flujo MPI.
     *
     * Bins usados:
     * - Rojo  → bin 0    ≈ 0°
     * - Verde → bin 12   ≈ 120°
     * - Azul  → bin 24   ≈ 240°
     */

    if (r >= g && r >= b) {
        return 0;
    }

    if (g >= r && g >= b) {
        return 12;
    }

    return 24;
}