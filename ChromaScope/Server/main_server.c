#include <mpi.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "mpi_protocol.h"
#include "../Nodes/worker.h"
#include "../Image/ppm.h"
#include "../Image/histogram.h"
#include "../Crypto/chacha20.h"

static void run_master(int world_size, const char *image_path);
static void fill_test_key_nonce(uint8_t key[CHACHA20_KEY_SIZE], uint8_t nonce[CHACHA20_NONCE_SIZE]);
static int write_binary_file(const char *path, const uint8_t *buffer, size_t size);

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
            fprintf(stderr, "  mpirun -np 4 ./chromascope_mpi test_image.ppm\n");
        }

        MPI_Finalize();
        return EXIT_FAILURE;
    }

    if (argc != 2) {
        if (rank == MASTER_RANK) {
            fprintf(stderr, "Uso:\n");
            fprintf(stderr, "  mpirun -np 4 ./chromascope_mpi imagen.ppm\n");
        }

        MPI_Finalize();
        return EXIT_FAILURE;
    }

    if (rank == MASTER_RANK) {
        run_master(world_size, argv[1]);
    } else {
        run_worker(rank);
    }

    MPI_Finalize();
    return EXIT_SUCCESS;
}

static void run_master(int world_size, const char *image_path) {
    PPMImage image;

    if (ppm_read(image_path, &image) != 0) {
        fprintf(stderr, "[Rank 0] Error: no se pudo leer la imagen PPM.\n");
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    printf("\n[Rank 0] Nodo maestro iniciado\n");
    printf("[Rank 0] Imagen PPM cargada: %dx%d RGB\n", image.width, image.height);
    printf("[Rank 0] Total de workers: %d\n\n", NUM_WORKERS);

    if (ppm_write_gray("cluster_output_gray.ppm", &image) != 0) {
        fprintf(stderr, "[Rank 0] Advertencia: no se pudo guardar imagen gris.\n");
    } else {
        printf("[Rank 0] Imagen gris guardada como cluster_output_gray.ppm\n");
    }

    const int width = image.width;
    const int height = image.height;
    const int channels = CHANNELS_RGB;

    size_t total_bytes = (size_t)width * (size_t)height * (size_t)channels;

    uint8_t *encrypted_pixels = malloc(total_bytes);
    uint8_t *decrypted_pixels = malloc(total_bytes);

    if (encrypted_pixels == NULL || decrypted_pixels == NULL) {
        fprintf(stderr, "[Rank 0] Error: no se pudo reservar memoria para cifrado.\n");
        free(encrypted_pixels);
        free(decrypted_pixels);
        ppm_free(&image);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    uint8_t session_key[CHACHA20_KEY_SIZE];
    uint8_t base_nonce[CHACHA20_NONCE_SIZE];

    fill_test_key_nonce(session_key, base_nonce);

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
        size_t strip_offset = (size_t)current_start_row * (size_t)width * (size_t)channels;

        StripMetadata metadata;
        metadata.width = width;
        metadata.height = height;
        metadata.start_row = current_start_row;
        metadata.rows = rows_for_worker;
        metadata.channels = channels;
        metadata.strip_bytes = strip_bytes;
        metadata.hist_bins = HIST_BINS;
        metadata.crypto_counter = 1;

        memcpy(metadata.key, session_key, CHACHA20_KEY_SIZE);
        memcpy(metadata.nonce, base_nonce, CHACHA20_NONCE_SIZE);

        /*
         * Para esta integración inicial se usa un nonce diferente por franja.
         * Así cada worker puede descifrar su franja de forma independiente.
         */
        metadata.nonce[CHACHA20_NONCE_SIZE - 1] ^= (uint8_t)worker_rank;

        const uint8_t *plain_strip = image.pixels + strip_offset;
        uint8_t *encrypted_strip = encrypted_pixels + strip_offset;
        uint8_t *decrypted_strip = decrypted_pixels + strip_offset;

        chacha20_encrypt_buffer(
            metadata.key,
            metadata.nonce,
            metadata.crypto_counter,
            plain_strip,
            encrypted_strip,
            (size_t)strip_bytes
        );

        /*
         * Verificación local del master: descifra también para confirmar que
         * la imagen cifrada por franjas puede recuperarse correctamente.
         */
        chacha20_encrypt_buffer(
            metadata.key,
            metadata.nonce,
            metadata.crypto_counter,
            encrypted_strip,
            decrypted_strip,
            (size_t)strip_bytes
        );

        printf("[Rank 0] Enviando franja cifrada a Rank %d\n", worker_rank);
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

        MPI_Send(encrypted_strip,
                 strip_bytes,
                 MPI_UNSIGNED_CHAR,
                 worker_rank,
                 TAG_IMAGE_STRIP,
                 MPI_COMM_WORLD);

        current_start_row += rows_for_worker;
    }

    if (write_binary_file("cluster_encrypted_pixels.bin", encrypted_pixels, total_bytes) == 0) {
        printf("[Rank 0] Pixeles cifrados guardados como cluster_encrypted_pixels.bin\n");
    }

    PPMImage decrypted_image;
    decrypted_image.width = width;
    decrypted_image.height = height;
    decrypted_image.pixels = decrypted_pixels;

    if (ppm_write("cluster_decrypted.ppm", &decrypted_image) == 0) {
        printf("[Rank 0] Imagen descifrada de verificacion guardada como cluster_decrypted.ppm\n");
    }

    uint32_t master_partial_histogram[HIST_BINS];
    uint32_t global_histogram[HIST_BINS];

    memset(master_partial_histogram, 0, sizeof(master_partial_histogram));
    memset(global_histogram, 0, sizeof(global_histogram));

    MPI_Reduce(master_partial_histogram,
               global_histogram,
               HIST_BINS,
               MPI_UINT32_T,
               MPI_SUM,
               MASTER_RANK,
               MPI_COMM_WORLD);

    printf("\n[Rank 0] Histogramas consolidados mediante MPI_Reduce.\n");

    printf("\n[Rank 0] Histograma HSV consolidado:\n");
    histogram_print(global_histogram, HIST_BINS);

    int dominant_bin = histogram_find_dominant_bin(global_histogram, HIST_BINS);

    printf("\n[Rank 0] Bin dominante: %d\n", dominant_bin);
    printf("[Rank 0] Rango aproximado de hue: %d° - %d°\n",
           dominant_bin * 10,
           (dominant_bin + 1) * 10);

    uint64_t total_histogram_pixels = 0;
    for (int i = 0; i < HIST_BINS; i++) {
        total_histogram_pixels += global_histogram[i];
    }

    printf("[Rank 0] Pixeles esperados: %d\n", width * height);
    printf("[Rank 0] Pixeles en histograma: %lu\n", total_histogram_pixels);

    if (total_histogram_pixels == (uint64_t)(width * height)) {
        printf("[Rank 0] Validacion OK: histograma cubre toda la imagen.\n");
    } else {
        printf("[Rank 0] Validacion FALLIDA: histograma no coincide con pixeles esperados.\n");
    }

    printf("\n[Rank 0] Integracion image + crypto + MPI completada.\n\n");

    free(encrypted_pixels);
    free(decrypted_pixels);
    ppm_free(&image);
}

static void fill_test_key_nonce(uint8_t key[CHACHA20_KEY_SIZE], uint8_t nonce[CHACHA20_NONCE_SIZE]) {
    for (int i = 0; i < CHACHA20_KEY_SIZE; i++) {
        key[i] = (uint8_t)i;
    }

    for (int i = 0; i < CHACHA20_NONCE_SIZE; i++) {
        nonce[i] = (uint8_t)(i + 100);
    }
}

static int write_binary_file(const char *path, const uint8_t *buffer, size_t size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: no se pudo crear archivo: %s\n", path);
        return -1;
    }

    size_t written = fwrite(buffer, 1, size, file);
    fclose(file);

    return written == size ? 0 : -1;
}