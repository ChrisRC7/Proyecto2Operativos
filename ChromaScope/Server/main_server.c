#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "mpi_protocol.h"
#include "../Nodes/worker.h"

static void fill_fake_image(uint8_t *image, int width, int height);
static void print_histogram(const uint32_t *histogram, int bins);
static int find_dominant_bin(const uint32_t *histogram, int bins);
static void run_master(int world_size);

int main(int argc, char **argv) {
    int rank;
    int world_size;

    MPI_Init(&argc, &argv);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    if (world_size != EXPECTED_MPI_PROCESSES) {
        if (rank == MASTER_RANK) {
            fprintf(stderr,
                    "Error: este programa debe ejecutarse con %d procesos MPI.\n",
                    EXPECTED_MPI_PROCESSES);
            fprintf(stderr, "Ejemplo:\n");
            fprintf(stderr, "  mpirun -np 4 ./chromascope_mpi\n");
        }

        MPI_Finalize();
        return EXIT_FAILURE;
    }

    if (rank == MASTER_RANK) {
        run_master(world_size);
    } else {
        run_worker(rank);
    }

    MPI_Finalize();
    return EXIT_SUCCESS;
}

static void run_master(int world_size) {
    const int width = 800;
    const int height = 600;
    const int channels = CHANNELS_RGB;
    const int total_bytes = width * height * channels;

    uint8_t *image = malloc((size_t)total_bytes);
    if (image == NULL) {
        fprintf(stderr, "[Rank 0] Error: no se pudo reservar memoria para imagen simulada.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    fill_fake_image(image, width, height);

    printf("\n[Rank 0] Nodo maestro iniciado\n");
    printf("[Rank 0] Imagen simulada creada: %dx%d RGB\n", width, height);
    printf("[Rank 0] Total de workers: %d\n\n", NUM_WORKERS);

    int base_rows = height / NUM_WORKERS;
    int remaining_rows = height % NUM_WORKERS;
    int current_start_row = 0;

    for (int worker_rank = 1; worker_rank < world_size; worker_rank++) {
        int worker_index = worker_rank - 1;

        int rows_for_worker = base_rows;
        if (worker_index < remaining_rows) {
            rows_for_worker++;
        }

        int strip_bytes = width * rows_for_worker * channels;

        StripMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.start_row = current_start_row;
        metadata.rows = rows_for_worker;
        metadata.channels = channels;
        metadata.strip_bytes = strip_bytes;
        metadata.hist_bins = HIST_BINS;

        uint8_t *strip_start = image + ((size_t)current_start_row * width * channels);

        printf("[Rank 0] Enviando franja a Rank %d\n", worker_rank);
        printf("         start_row=%d, rows=%d, bytes=%d\n",
               metadata.start_row,
               metadata.rows,
               metadata.strip_bytes);

        MPI_Send(&metadata,
                 sizeof(StripMetadata),
                 MPI_BYTE,
                 worker_rank,
                 TAG_METADATA,
                 MPI_COMM_WORLD);

        MPI_Send(strip_start,
                 strip_bytes,
                 MPI_UNSIGNED_CHAR,
                 worker_rank,
                 TAG_IMAGE_STRIP,
                 MPI_COMM_WORLD);

        current_start_row += rows_for_worker;
    }

    uint32_t global_histogram[HIST_BINS];
    memset(global_histogram, 0, sizeof(global_histogram));

    for (int worker_rank = 1; worker_rank < world_size; worker_rank++) {
        uint32_t partial_histogram[HIST_BINS];

        MPI_Recv(partial_histogram,
                 HIST_BINS,
                 MPI_UINT32_T,
                 worker_rank,
                 TAG_HISTOGRAM,
                 MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);

        printf("[Rank 0] Histograma parcial recibido desde Rank %d\n", worker_rank);

        for (int i = 0; i < HIST_BINS; i++) {
            global_histogram[i] += partial_histogram[i];
        }
    }

    printf("\n[Rank 0] Histograma consolidado:\n");
    print_histogram(global_histogram, HIST_BINS);

    int dominant_bin = find_dominant_bin(global_histogram, HIST_BINS);

    printf("\n[Rank 0] Bin dominante: %d\n", dominant_bin);
    printf("[Rank 0] Rango aproximado de hue: %d° - %d°\n",
           dominant_bin * 10,
           (dominant_bin + 1) * 10);

    printf("\n[Rank 0] Fase 1 completada correctamente.\n");
    printf("[Rank 0] Siguiente paso: reemplazar imagen simulada por parser PPM real.\n\n");

    free(image);
}

static void fill_fake_image(uint8_t *image, int width, int height) {
    /*
     * Imagen simulada RGB.
     *
     * Se generan 3 zonas:
     * - Parte superior: rojo dominante
     * - Parte media: verde dominante
     * - Parte inferior: azul dominante
     *
     * Esto permite verificar que los workers realmente procesen distintas franjas.
     */

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            size_t index = ((size_t)y * width + x) * CHANNELS_RGB;

            if (y < height / 3) {
                image[index + 0] = 220;
                image[index + 1] = 40;
                image[index + 2] = 40;
            } else if (y < 2 * height / 3) {
                image[index + 0] = 40;
                image[index + 1] = 220;
                image[index + 2] = 40;
            } else {
                image[index + 0] = 40;
                image[index + 1] = 40;
                image[index + 2] = 220;
            }
        }
    }
}

static void print_histogram(const uint32_t *histogram, int bins) {
    for (int i = 0; i < bins; i++) {
        if (histogram[i] > 0) {
            printf("  Bin %02d: %u\n", i, histogram[i]);
        }
    }
}

static int find_dominant_bin(const uint32_t *histogram, int bins) {
    int dominant_bin = 0;
    uint32_t max_value = histogram[0];

    for (int i = 1; i < bins; i++) {
        if (histogram[i] > max_value) {
            max_value = histogram[i];
            dominant_bin = i;
        }
    }

    return dominant_bin;
}