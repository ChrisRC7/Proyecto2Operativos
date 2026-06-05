#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "../Server/mpi_protocol.h"
#include "../Crypto/chacha20.h"
#include "../Image/histogram.h"
#include "../Image/rgb_filter.h"

#include "worker.h"
#include "resource_monitor.h"

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

    uint8_t *encrypted_strip = malloc((size_t)metadata.strip_bytes);
    uint8_t *decrypted_strip = malloc((size_t)metadata.strip_bytes);
    uint8_t *filtered_strip = malloc((size_t)metadata.strip_bytes);

    if (encrypted_strip == NULL || decrypted_strip == NULL || filtered_strip == NULL) {
        fprintf(stderr, "[Rank %d] Error: no se pudo reservar memoria para las franjas.\n", rank);

        free(encrypted_strip);
        free(decrypted_strip);
        free(filtered_strip);

        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    MPI_Recv(encrypted_strip,
             metadata.strip_bytes,
             MPI_UNSIGNED_CHAR,
             MASTER_RANK,
             TAG_IMAGE_STRIP,
             MPI_COMM_WORLD,
             MPI_STATUS_IGNORE);

    printf("[Rank %d] Franja cifrada recibida correctamente.\n", rank);

    /*
     * ChaCha20 usa XOR, por eso la misma función sirve para cifrar y descifrar.
     * Master cifra:     plain XOR keystream = encrypted
     * Worker descifra:  encrypted XOR keystream = plain
     */
    chacha20_encrypt_buffer(
        metadata.key,
        metadata.nonce,
        metadata.crypto_counter,
        encrypted_strip,
        decrypted_strip,
        (size_t)metadata.strip_bytes
    );

    printf("[Rank %d] Franja descifrada correctamente con ChaCha20.\n", rank);

    print_resource_snapshot(rank);

    /*
     * El histograma debe calcularse sobre la imagen original.
     * Por eso se usa decrypted_strip ANTES de aplicar el filtro RGB.
     */
    uint32_t partial_histogram[HIST_BINS];
    memset(partial_histogram, 0, sizeof(partial_histogram));

    hsv_histogram_rgb_buffer(
        decrypted_strip,
        metadata.width,
        metadata.rows,
        metadata.hist_bins,
        partial_histogram
    );

    printf("[Rank %d] Histograma HSV parcial calculado sobre imagen original.\n", rank);

    /*
     * El filtro RGB lo aplica el worker.
     * La salida filtered_strip será devuelta al master para reconstruir la imagen completa.
     */
    rgb_filter_buffer(
        decrypted_strip,
        filtered_strip,
        metadata.width,
        metadata.rows,
        metadata.filter_r_gain,
        metadata.filter_g_gain,
        metadata.filter_b_gain
    );

    printf("[Rank %d] Filtro RGB aplicado sobre la franja.\n", rank);

    /*
     * Consolidación del histograma.
     * Cada worker aporta su histograma parcial.
     * El Rank 0 recibe el histograma global.
     */
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

    /*
     * Después de reducir el histograma, el worker devuelve al master
     * la franja ya filtrada.
     */
    MPI_Send(filtered_strip,
             metadata.strip_bytes,
             MPI_UNSIGNED_CHAR,
             MASTER_RANK,
             TAG_FILTERED_STRIP,
             MPI_COMM_WORLD);

    printf("[Rank %d] Franja filtrada enviada al maestro.\n", rank);

    free(encrypted_strip);
    free(decrypted_strip);
    free(filtered_strip);
}