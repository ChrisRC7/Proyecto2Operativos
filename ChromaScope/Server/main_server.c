#define _DEFAULT_SOURCE

#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <errno.h>

#include "mpi_protocol.h"
#include "../Nodes/worker.h"
#include "../Image/ppm.h"
#include "../Image/histogram.h"
#include "../Crypto/chacha20.h"
#include "../driver/chromascope_lcd.h"
#include "../driver/chromascope_gpio.h"

static void run_master(int world_size, const char *image_path);
static void fill_test_key_nonce(uint8_t key[CHACHA20_KEY_SIZE], uint8_t nonce[CHACHA20_NONCE_SIZE]);
static int display_histogram_on_lcd(const uint32_t *histogram, int bins);
static int gpio_completion_sweep(void);
static int write_binary_file(const char *path, const uint8_t *buffer, size_t size);

int main(int argc, char **argv) {
    int rank;
    int world_size;

    MPI_Init(&argc, &argv);

    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostname[sizeof(hostname) - 1] = '\0';
        printf("[Rank %d] Ejecutándose en host: %s\n", rank, hostname);
        fflush(stdout);
    }

    if (world_size < MIN_MPI_PROCESSES) {
        if (rank == MASTER_RANK) {
            fprintf(stderr,
                    "Error: este programa debe ejecutarse con al menos %d procesos MPI.\n",
                    MIN_MPI_PROCESSES);
            fprintf(stderr, "Ejemplo local:\n");
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
        fprintf(stderr, "[Rank 0] Error: no se pudo leer la imagen PPM: %s\n", image_path);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    const int width = image.width;
    const int height = image.height;
    const int channels = CHANNELS_RGB;
    const size_t total_bytes = (size_t)width * (size_t)height * (size_t)channels;
    const int num_workers = world_size - 1;

    printf("\n[Rank 0] Nodo maestro iniciado\n");
    printf("[Rank 0] Imagen PPM cargada: %dx%d RGB\n", width, height);
    printf("[Rank 0] Total de workers: %d\n", num_workers);
    printf("[Rank 0] Filtro RGB configurado: inversion de colores por canal RGB\n\n");

    uint8_t *encrypted_pixels = malloc(total_bytes);
    uint8_t *decrypted_pixels = malloc(total_bytes);
    uint8_t *filtered_pixels = malloc(total_bytes);

    if (encrypted_pixels == NULL || decrypted_pixels == NULL || filtered_pixels == NULL) {
        fprintf(stderr, "[Rank 0] Error: no se pudo reservar memoria para buffers.\n");

        free(encrypted_pixels);
        free(decrypted_pixels);
        free(filtered_pixels);
        ppm_free(&image);

        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    /*
     * Se guarda metadata por rank para poder reconstruir la imagen filtrada.
     * Esto es importante para evitar líneas divisorias o franjas mal ubicadas.
     */
    StripMetadata *metadata_by_rank = calloc((size_t)world_size, sizeof(StripMetadata));

    if (metadata_by_rank == NULL) {
        fprintf(stderr, "[Rank 0] Error: no se pudo reservar metadata_by_rank.\n");

        free(encrypted_pixels);
        free(decrypted_pixels);
        free(filtered_pixels);
        ppm_free(&image);

        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    uint8_t session_key[CHACHA20_KEY_SIZE];
    uint8_t base_nonce[CHACHA20_NONCE_SIZE];

    fill_test_key_nonce(session_key, base_nonce);

    int base_rows = height / num_workers;
    int remaining_rows = height % num_workers;
    int current_start_row = 0;

    for (int worker_rank = 1; worker_rank < world_size; worker_rank++) {
        int worker_index = worker_rank - 1;

        int rows_for_worker = base_rows;
        if (worker_index < remaining_rows) {
            rows_for_worker++;
        }

        int strip_bytes = width * rows_for_worker * channels;

        size_t strip_offset =
            (size_t)current_start_row *
            (size_t)width *
            (size_t)channels;

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
         * Cada worker usa un nonce ligeramente distinto.
         * Esto evita reutilizar el mismo keystream para franjas distintas.
         */
        metadata.nonce[CHACHA20_NONCE_SIZE - 1] ^= (uint8_t)worker_rank;

        /*
         * Parámetros del filtro RGB.
         * Este filtro lo aplican los workers, no el master.
         */
        metadata.filter_r_gain = 1.15f;
        metadata.filter_g_gain = 1.00f;
        metadata.filter_b_gain = 0.85f;

        metadata_by_rank[worker_rank] = metadata;

        const uint8_t *plain_strip = image.pixels + strip_offset;
        uint8_t *encrypted_strip = encrypted_pixels + strip_offset;
        uint8_t *decrypted_strip = decrypted_pixels + strip_offset;

        /*
         * El master cifra la franja original antes de enviarla.
         */
        chacha20_encrypt_buffer(
            metadata.key,
            metadata.nonce,
            metadata.crypto_counter,
            plain_strip,
            encrypted_strip,
            (size_t)strip_bytes
        );

        /*
         * Verificación local opcional:
         * se descifra la franja cifrada para comprobar que puede recuperarse.
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
        fflush(stdout);

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

    /*
     * Guardar archivo cifrado y descifrado de verificación.
     * cluster_encrypted_pixels.bin no es un PPM completo, solo los bytes RGB cifrados.
     */
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

    /*
     * El master participa en MPI_Reduce con un histograma vacío.
     * Los workers son quienes calculan histogramas reales de sus franjas originales.
     */
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

    /*
     * Después del histograma, el master recibe las franjas filtradas.
     * Cada franja se coloca usando start_row, no por orden de llegada.
     * Esto evita líneas divisorias o franjas mal ubicadas.
     */
    printf("[Rank 0] Recibiendo franjas filtradas desde workers...\n");

    for (int worker_rank = 1; worker_rank < world_size; worker_rank++) {
        StripMetadata metadata = metadata_by_rank[worker_rank];

        size_t strip_offset =
            (size_t)metadata.start_row *
            (size_t)metadata.width *
            (size_t)metadata.channels;

        MPI_Recv(filtered_pixels + strip_offset,
                 metadata.strip_bytes,
                 MPI_UNSIGNED_CHAR,
                 worker_rank,
                 TAG_FILTERED_STRIP,
                 MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);

        printf("[Rank 0] Franja filtrada recibida desde Rank %d: start_row=%d, rows=%d\n",
               worker_rank,
               metadata.start_row,
               metadata.rows);
    }

    PPMImage filtered_image;
    filtered_image.width = width;
    filtered_image.height = height;
    filtered_image.pixels = filtered_pixels;

    if (ppm_write("cluster_filtered_rgb.ppm", &filtered_image) == 0) {
        printf("[Rank 0] Imagen filtrada RGB reconstruida guardada como cluster_filtered_rgb.ppm\n");
    } else {
        fprintf(stderr, "[Rank 0] Error: no se pudo guardar cluster_filtered_rgb.ppm\n");
    }

    printf("\n[Rank 0] Histograma HSV consolidado basado en imagen original:\n");
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
        printf("[Rank 0] Validacion OK: histograma cubre toda la imagen original.\n");
    } else {
        printf("[Rank 0] Validacion FALLIDA: histograma no coincide con pixeles esperados.\n");
    }

    printf("\n[Rank 0] Integracion filtro RGB + histograma original + MPI completada.\n");

    if (display_histogram_on_lcd(global_histogram, HIST_BINS) == 0) {
        printf("[Rank 0] Histograma mostrado en la LCD ST7735.\n");
    } else {
        printf("[Rank 0] No se pudo mostrar el histograma en la LCD.\n");
    }

    if (gpio_completion_sweep() == 0) {
        printf("[Rank 0] Barrido GPIO de finalizacion completado.\n");
    } else {
        printf("[Rank 0] No se pudo ejecutar el barrido GPIO.\n");
    }

    printf("\n");

    free(metadata_by_rank);
    free(encrypted_pixels);
    free(decrypted_pixels);
    free(filtered_pixels);
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

static int display_histogram_on_lcd(const uint32_t *histogram, int bins) {
    int fd = open(LCD_DEV, O_RDWR);
    if (fd < 0) {
        fprintf(stderr, "[Rank 0] No se pudo abrir %s: %s\n", LCD_DEV, strerror(errno));
        return -1;
    }

    if (ioctl(fd, IOCTL_LCD_INIT) < 0) {
        fprintf(stderr, "[Rank 0] IOCTL_LCD_INIT falló: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct lcd_cmd_histogram cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.n_bins = (bins > LCD_MAX_BINS) ? LCD_MAX_BINS : bins;
    for (int i = 0; i < cmd.n_bins; i++) {
        cmd.bins[i] = histogram[i];
    }

    if (ioctl(fd, IOCTL_LCD_SHOW_HISTOGRAM, &cmd) < 0) {
        fprintf(stderr, "[Rank 0] IOCTL_LCD_SHOW_HISTOGRAM falló: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static int gpio_completion_sweep(void) {
    const int servo_pin = 18; /* Cambiar según el pin BCM usado para el SG90 */
    int fd = open(CHROMASCOPE_DEV, O_RDWR);

    if (fd < 0) {
        fprintf(stderr, "[Rank 0] No se pudo abrir %s: %s\n", CHROMASCOPE_DEV, strerror(errno));
        return -1;
    }

    struct gpio_cmd_servo cmd;

    cmd.pin = servo_pin;
    cmd.angle = 0;
    cmd.duration_ms = 400;
    if (ioctl(fd, IOCTL_GPIO_SET_SERVO_ANGLE, &cmd) < 0) {
        fprintf(stderr, "[Rank 0] IOCTL_GPIO_SET_SERVO_ANGLE(0) falló: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    usleep(500000); /* esperar a que el servo alcance la posición 0° */

    cmd.angle = 90;
    cmd.duration_ms = 800;
    if (ioctl(fd, IOCTL_GPIO_SET_SERVO_ANGLE, &cmd) < 0) {
        fprintf(stderr, "[Rank 0] IOCTL_GPIO_SET_SERVO_ANGLE(90) falló: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    usleep(800000); /* mantener 90° un momento */

    cmd.angle = 0;
    cmd.duration_ms = 800;
    if (ioctl(fd, IOCTL_GPIO_SET_SERVO_ANGLE, &cmd) < 0) {
        fprintf(stderr, "[Rank 0] IOCTL_GPIO_SET_SERVO_ANGLE(0) falló: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
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