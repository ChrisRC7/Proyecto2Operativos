/* main_server.c — Proceso maestro del clúster ChromaScope (Rank 0).
 *
 * Flujo completo del master:
 *   1. Inicializar MPI y obtener rank/tamaño del comunicador.
 *   2. Leer la imagen PPM del disco en un buffer de píxeles RGB.
 *   3. Para cada worker: dividir la imagen en franjas horizontales.
 *   4. Cifrar cada franja con ChaCha20 usando un nonce único por worker.
 *   5. Enviar metadata + franja cifrada a cada worker (MPI_Send).
 *   6. Guardar la imagen cifrada en disco (modo debug).
 *   7. Participar en MPI_Reduce para recolectar histogramas parciales.
 *   8. Recibir las franjas filtradas de cada worker.
 *   9. Reconstruir y guardar la imagen filtrada completa.
 *  10. Mostrar el histograma en la pantalla LCD ST7735.
 *  11. Ejecutar el barrido de servo SG90 como señal de finalización.
 *  12. Liberar toda la memoria y finalizar MPI.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

/* _DEFAULT_SOURCE: necesario para que glibc exponga gethostname() en <unistd.h>*/
#define _DEFAULT_SOURCE

#include <mpi.h>        /* API completa de MPI: Init, Finalize, Send, Recv, etc.*/
#include <stdio.h>      /* printf, fprintf, fopen, fclose, fwrite               */
#include <stdlib.h>     /* malloc, calloc, free, EXIT_FAILURE, getenv          */
#include <stdint.h>     /* uint8_t, uint32_t, uint64_t: tipos de ancho fijo    */
#include <string.h>     /* memcpy, memset: manipulación de bloques de memoria   */
#include <unistd.h>     /* gethostname(), usleep(): hostname y espera en µs    */
#include <errno.h>      /* errno, strerror(): diagnóstico de errores del SO    */

#include "mpi_protocol.h"            /* StripMetadata, tags, constantes de procesos */
#include "../Nodes/worker.h"         /* run_worker(): punto de entrada del worker   */
#include "../Image/ppm.h"            /* PPMImage, ppm_read, ppm_write, ppm_free     */
#include "../Image/histogram.h"      /* histogram_print, histogram_find_dominant_bin*/
#include "../Crypto/chacha20.h"      /* chacha20_encrypt_buffer()                   */

/*
 * Bibliotecas de hardware: son la ÚNICA interfaz permitida con los drivers.
 * No se usan ioctl, open ni close directos en este archivo; toda la
 * interacción con el hardware queda encapsulada en las funciones lib*.
 */
#include "../lib/libchromascope_lcd.h"  /* lcd_fill_screen, lcd_show_histogram, etc. */
#include "../lib/libchromascope_gpio.h" /* gpio_servo_move, chromascope_gpio_init, etc.*/

/* ── Declaraciones de funciones privadas ────────────────────────────────── */
static void run_master(int world_size, const char *image_path);
static void fill_test_key_nonce(uint8_t key[CHACHA20_KEY_SIZE], uint8_t nonce[CHACHA20_NONCE_SIZE]);
static int  display_histogram_on_lcd(const uint32_t *histogram, int bins);
static int  gpio_completion_sweep(void);
static int  write_binary_file(const char *path, const uint8_t *buffer, size_t size);

/*
 * Nota: display_histogram_on_lcd() y gpio_completion_sweep() son wrappers
 * delgados sobre las funciones de las bibliotecas libchromascope_lcd y
 * libchromascope_gpio. Su lógica interna ya no contiene ioctl directos.
 */

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * main() — punto de entrada: inicializa MPI y despacha master o worker.
 *
 * Todos los procesos MPI ejecutan este mismo binario. El rank determina el rol:
 *   rank == 0 → maestro (run_master)
 *   rank >= 1 → worker  (run_worker)
 */
int main(int argc, char **argv) {

    int rank;        /* identificador único de este proceso en MPI_COMM_WORLD   */
    int world_size;  /* número total de procesos lanzados con mpirun -np N      */

    /* MPI_Init inicializa el runtime de MPI y procesa los argumentos de línea.  */
    MPI_Init(&argc, &argv);

    /* Obtener el rank de este proceso (0 = master, 1..N-1 = workers).          */
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    /* Obtener el número total de procesos en el comunicador global.             */
    MPI_Comm_size(MPI_COMM_WORLD, &world_size);

    /* Mostrar en qué máquina física corre cada proceso (útil en clúster multi-nodo).*/
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        hostname[sizeof(hostname) - 1] = '\0';  /* garantizar terminación null  */
        printf("[Rank %d] Ejecutándose en host: %s\n", rank, hostname);
        fflush(stdout);  /* vaciar buffer: en MPI la salida puede quedar en buffer*/
    }

    /* Verificar que hay suficientes procesos para el modelo master-worker.     */
    if (world_size < MIN_MPI_PROCESSES) {
        if (rank == MASTER_RANK) {
            fprintf(stderr,
                    "Error: este programa debe ejecutarse con al menos %d procesos MPI.\n",
                    MIN_MPI_PROCESSES);
            fprintf(stderr, "Ejemplo local:\n");
            fprintf(stderr, "  mpirun -np 4 ./chromascope_mpi test_image.ppm\n");
        }

        MPI_Finalize();    /* limpiar el runtime MPI antes de salir              */
        return EXIT_FAILURE;
    }

    /* Verificar que se pasó exactamente un argumento: el path de la imagen.    */
    if (argc != 2) {
        if (rank == MASTER_RANK) {
            fprintf(stderr, "Uso:\n");
            fprintf(stderr, "  mpirun -np 4 ./chromascope_mpi imagen.ppm\n");
        }

        MPI_Finalize();
        return EXIT_FAILURE;
    }

    /* Despachar al rol correspondiente según el rank.                           */
    if (rank == MASTER_RANK) {
        run_master(world_size, argv[1]);  /* argv[1] = ruta de la imagen PPM   */
    } else {
        run_worker(rank);                 /* workers: esperan instrucciones     */
    }

    /* MPI_Finalize: sincroniza todos los procesos y limpia recursos MPI.        */
    MPI_Finalize();
    return EXIT_SUCCESS;
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * run_master() — lógica completa del proceso maestro.
 *
 * Divide la imagen en franjas horizontales, cifra cada una, la distribuye
 * entre workers mediante MPI, recolecta resultados y accede a los periféricos.
 */
static void run_master(int world_size, const char *image_path) {

    PPMImage image;   /* estructura que contiene dimensiones y array de píxeles */

    /* Variable de modo benchmark: si la variable de entorno CHROMA_BENCH está
     * definida, se omiten los archivos de salida y los accesos a periféricos.   */
    const int bench_mode = (getenv("CHROMA_BENCH") != NULL);

    /* ── Paso 1: Leer la imagen PPM del disco ── */
    if (ppm_read(image_path, &image) != 0) {
        fprintf(stderr, "[Rank 0] Error: no se pudo leer la imagen PPM: %s\n", image_path);
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);  /* terminar todos los procesos */
    }

    /* Extraer dimensiones y calcular tamaño total en bytes.                    */
    const int width        = image.width;
    const int height       = image.height;
    const int channels     = CHANNELS_RGB;                        /* siempre 3   */
    const size_t total_bytes = (size_t)width * (size_t)height * (size_t)channels;
    const int num_workers  = world_size - 1;  /* workers = total - 1 master    */

    printf("\n[Rank 0] Nodo maestro iniciado\n");
    printf("[Rank 0] Imagen PPM cargada: %dx%d RGB\n", width, height);
    printf("[Rank 0] Total de workers: %d\n", num_workers);
    printf("[Rank 0] Filtro RGB configurado: inversion de colores por canal RGB\n\n");

    /* ── Paso 2: Reservar tres buffers del tamaño completo de la imagen ── */
    uint8_t *encrypted_pixels = malloc(total_bytes);   /* imagen cifrada        */
    uint8_t *decrypted_pixels = malloc(total_bytes);   /* verificación local    */
    uint8_t *filtered_pixels  = malloc(total_bytes);   /* imagen filtrada final */

    if (encrypted_pixels == NULL || decrypted_pixels == NULL || filtered_pixels == NULL) {
        fprintf(stderr, "[Rank 0] Error: no se pudo reservar memoria para buffers.\n");

        free(encrypted_pixels);
        free(decrypted_pixels);
        free(filtered_pixels);
        ppm_free(&image);

        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    /*
     * metadata_by_rank guarda la metadata de cada worker para poder reconstruir
     * la imagen en el orden correcto al recibir las franjas filtradas.
     * Se indexa por rank (0 = master no tiene franja, 1..N = workers).
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

    /* ── Paso 3: Generar la clave y nonce de sesión ChaCha20 ── */
    uint8_t session_key[CHACHA20_KEY_SIZE];
    uint8_t base_nonce[CHACHA20_NONCE_SIZE];

    /* Rellenar con valores de prueba (0,1,2,...) + offset para el nonce.       */
    fill_test_key_nonce(session_key, base_nonce);

    /* ── Paso 4: Calcular el reparto de filas entre workers ── */
    int base_rows      = height / num_workers;    /* filas base por worker      */
    int remaining_rows = height % num_workers;    /* filas sobrantes            */
    int current_start_row = 0;                    /* cursor de fila actual      */

    /* Inicio de la medición de tiempo MPI: justo antes del cifrado y envío.   */
    double t_start = MPI_Wtime();

    /* ── Paso 5: Distribuir franjas cifradas a cada worker ── */
    for (int worker_rank = 1; worker_rank < world_size; worker_rank++) {
        int worker_index = worker_rank - 1;  /* índice 0-based del worker       */

        /* Distribuir las filas sobrantes: los primeros 'remaining_rows' workers
         * reciben una fila extra para balancear la carga uniformemente.         */
        int rows_for_worker = base_rows;
        if (worker_index < remaining_rows) {
            rows_for_worker++;  /* este worker recibe una fila extra             */
        }

        int strip_bytes = width * rows_for_worker * channels;  /* bytes totales */

        /* Offset en bytes dentro del array lineal de la imagen completa.       */
        size_t strip_offset =
            (size_t)current_start_row *
            (size_t)width *
            (size_t)channels;

        /* ── Rellenar la metadata de este worker ── */
        StripMetadata metadata;

        metadata.width       = width;            /* ancho de la imagen completa */
        metadata.height      = height;           /* alto de la imagen completa  */
        metadata.start_row   = current_start_row; /* primera fila de la franja  */
        metadata.rows        = rows_for_worker;  /* número de filas asignadas   */
        metadata.channels    = channels;         /* bytes por píxel (3)         */
        metadata.strip_bytes = strip_bytes;      /* bytes totales de la franja  */
        metadata.hist_bins   = HIST_BINS;        /* 36 bins para 10°/bin        */

        /* Contador inicial de bloque ChaCha20 (1 por convención del RFC 8439). */
        metadata.crypto_counter = 1;

        /* Copiar la clave y nonce base a la metadata.                          */
        memcpy(metadata.key,   session_key, CHACHA20_KEY_SIZE);
        memcpy(metadata.nonce, base_nonce,  CHACHA20_NONCE_SIZE);

        /*
         * Personalizar el nonce para cada worker: XOR del último byte con el rank.
         * Esto garantiza que cada franja usa un keystream ChaCha20 diferente.
         * Reutilizar el mismo nonce con la misma clave en ChaCha20 es un fallo
         * criptográfico grave (permite recuperar el keystream por XOR de ciphertexts).
         */
        metadata.nonce[CHACHA20_NONCE_SIZE - 1] ^= (uint8_t)worker_rank;

        /* Parámetros del filtro RGB (ganancias por canal).                     */
        metadata.filter_r_gain = 1.15f;  /* rojo: ligero aumento               */
        metadata.filter_g_gain = 1.00f;  /* verde: sin cambio                  */
        metadata.filter_b_gain = 0.85f;  /* azul: ligera reducción             */

        /* Guardar metadata para usarla al recibir las franjas filtradas.       */
        metadata_by_rank[worker_rank] = metadata;

        /* Punteros a las sub-regiones de los buffers globales.                 */
        const uint8_t *plain_strip     = image.pixels       + strip_offset;
        uint8_t       *encrypted_strip = encrypted_pixels   + strip_offset;
        uint8_t       *decrypted_strip = decrypted_pixels   + strip_offset;

        /*
         * Cifrar la franja original con ChaCha20 antes de enviarla al worker.
         * El master actúa como "emisor seguro": envía datos cifrados, no en claro.
         */
        chacha20_encrypt_buffer(
            metadata.key,
            metadata.nonce,
            metadata.crypto_counter,
            plain_strip,        /* input: píxeles RGB originales                */
            encrypted_strip,    /* output: píxeles cifrados con XOR keystream   */
            (size_t)strip_bytes
        );

        /*
         * Verificación local: descifrar la franja recién cifrada para confirmar
         * que el cifrado es reversible. En ChaCha20: (plain XOR ks) XOR ks = plain.
         * Si encrypted XOR ks ≠ plain, hay un bug en la implementación.
         */
        chacha20_encrypt_buffer(
            metadata.key,
            metadata.nonce,
            metadata.crypto_counter,
            encrypted_strip,    /* input: cifrado                               */
            decrypted_strip,    /* output: debería ser igual a plain_strip      */
            (size_t)strip_bytes
        );

        printf("[Rank 0] Enviando franja cifrada a Rank %d\n", worker_rank);
        printf("         start_row=%d, rows=%d, bytes=%d\n",
               metadata.start_row, metadata.rows, metadata.strip_bytes);
        fflush(stdout);

        /* Enviar la metadata al worker (TAG_METADATA = primer mensaje).        */
        MPI_Send(
            &metadata,
            sizeof(StripMetadata),
            MPI_BYTE,           /* enviar como bytes crudos sin interpretación  */
            worker_rank,        /* destino: el worker correspondiente           */
            TAG_METADATA,       /* tag para identificar el tipo de mensaje      */
            MPI_COMM_WORLD
        );

        /* Enviar la franja de píxeles cifrados (TAG_IMAGE_STRIP = segundo msg).*/
        MPI_Send(
            encrypted_strip,
            strip_bytes,
            MPI_UNSIGNED_CHAR,  /* array de bytes sin signo                     */
            worker_rank,
            TAG_IMAGE_STRIP,
            MPI_COMM_WORLD
        );

        current_start_row += rows_for_worker;  /* avanzar al inicio de la siguiente franja */
    }

    /* Guardar imagen cifrada en disco para análisis (solo en modo normal).     */
    if (!bench_mode) {
        if (write_binary_file("cluster_encrypted_pixels.bin", encrypted_pixels, total_bytes) == 0) {
            printf("[Rank 0] Pixeles cifrados guardados como cluster_encrypted_pixels.bin\n");
        }
    }

    /* Guardar la imagen descifrada de verificación como archivo PPM.           */
    PPMImage decrypted_image;
    decrypted_image.width  = width;
    decrypted_image.height = height;
    decrypted_image.pixels = decrypted_pixels;  /* apunta al buffer existente   */

    if (ppm_write("cluster_decrypted.ppm", &decrypted_image) == 0) {
        printf("[Rank 0] Imagen descifrada de verificacion guardada como cluster_decrypted.ppm\n");
    }

    /* ── Paso 6: Participar en MPI_Reduce para recolectar histogramas ── */
    /*
     * MPI_Reduce es colectiva: el master también debe llamarla.
     * El master contribuye con un histograma vacío (todos ceros) porque
     * no procesa ninguna franja de imagen.
     */
    uint32_t master_partial_histogram[HIST_BINS];
    uint32_t global_histogram[HIST_BINS];

    memset(master_partial_histogram, 0, sizeof(master_partial_histogram));
    memset(global_histogram,         0, sizeof(global_histogram));

    MPI_Reduce(
        master_partial_histogram,  /* contribución del master: todo ceros       */
        global_histogram,          /* buffer de recepción del histograma global  */
        HIST_BINS,                 /* número de bins a reducir                  */
        MPI_UINT32_T,              /* tipo: entero sin signo de 32 bits         */
        MPI_SUM,                   /* operación: sumar todos los valores        */
        MASTER_RANK,               /* quien recibe el resultado (rank 0)        */
        MPI_COMM_WORLD
    );

    printf("\n[Rank 0] Histogramas consolidados mediante MPI_Reduce.\n");

    /* ── Paso 7: Recibir las franjas filtradas de cada worker ── */
    /*
     * El master recibe las franjas en cualquier orden pero las coloca usando
     * metadata.start_row para garantizar que la imagen se reconstruye correctamente.
     * No se asume que los workers responden en el orden en que se les envió trabajo.
     */
    printf("[Rank 0] Recibiendo franjas filtradas desde workers...\n");

    for (int worker_rank = 1; worker_rank < world_size; worker_rank++) {
        StripMetadata metadata = metadata_by_rank[worker_rank];

        /* Calcular el offset donde va esta franja en el buffer global.         */
        size_t strip_offset =
            (size_t)metadata.start_row *
            (size_t)metadata.width *
            (size_t)metadata.channels;

        /* Recibir la franja filtrada y escribirla en la posición correcta.     */
        MPI_Recv(
            filtered_pixels + strip_offset,  /* destino en el buffer global    */
            metadata.strip_bytes,            /* bytes esperados                 */
            MPI_UNSIGNED_CHAR,
            worker_rank,
            TAG_FILTERED_STRIP,             /* tag específico para franjas filtradas */
            MPI_COMM_WORLD,
            MPI_STATUS_IGNORE
        );

        printf("[Rank 0] Franja filtrada recibida desde Rank %d: start_row=%d, rows=%d\n",
               worker_rank, metadata.start_row, metadata.rows);
    }

    /* Fin de la medición de tiempo: toda la comunicación MPI completada.       */
    double t_end = MPI_Wtime();
    /* Formato "tiempo=X.XXXX" parseable por el script run_cluster.sh para benchmark. */
    printf("tiempo=%.4f\n", t_end - t_start);
    fflush(stdout);

    /* ── Paso 8: Guardar resultados y usar periféricos (solo modo normal) ── */
    if (!bench_mode) {

        /* Reconstruir y guardar la imagen filtrada completa.                   */
        PPMImage filtered_image;
        filtered_image.width  = width;
        filtered_image.height = height;
        filtered_image.pixels = filtered_pixels;

        if (ppm_write("cluster_filtered_rgb.ppm", &filtered_image) == 0) {
            printf("[Rank 0] Imagen filtrada RGB reconstruida guardada como cluster_filtered_rgb.ppm\n");
        } else {
            fprintf(stderr, "[Rank 0] Error: no se pudo guardar cluster_filtered_rgb.ppm\n");
        }

        /* Imprimir el histograma HSV global consolidado en consola.            */
        printf("\n[Rank 0] Histograma HSV consolidado basado en imagen original:\n");
        histogram_print(global_histogram, HIST_BINS);

        /* Encontrar el bin dominante (color más frecuente en la imagen).       */
        int dominant_bin = histogram_find_dominant_bin(global_histogram, HIST_BINS);
        printf("\n[Rank 0] Bin dominante: %d\n", dominant_bin);
        printf("[Rank 0] Rango aproximado de hue: %d° - %d°\n",
               dominant_bin * 10, (dominant_bin + 1) * 10);

        /* Validar que el histograma cubre exactamente todos los píxeles.       */
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

        /* Mostrar el histograma en la pantalla LCD ST7735 via ioctl.           */
        if (display_histogram_on_lcd(global_histogram, HIST_BINS) == 0) {
            printf("[Rank 0] Histograma mostrado en la LCD ST7735.\n");
        } else {
            printf("[Rank 0] No se pudo mostrar el histograma en la LCD.\n");
        }

        /* Ejecutar el barrido de servo como señal visual de finalización.      */
        if (gpio_completion_sweep() == 0) {
            printf("[Rank 0] Barrido GPIO de finalizacion completado.\n");
        } else {
            printf("[Rank 0] No se pudo ejecutar el barrido GPIO.\n");
        }
    }

    printf("\n");

    /* ── Paso 9: Liberar todos los recursos del master ── */
    free(metadata_by_rank);    /* array de structs de metadata                  */
    free(encrypted_pixels);    /* buffer de imagen cifrada                      */
    free(decrypted_pixels);    /* buffer de verificación de descifrado          */
    free(filtered_pixels);     /* buffer de imagen filtrada reconstruida        */
    ppm_free(&image);          /* liberar el buffer de la imagen original        */
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * fill_test_key_nonce() — rellena la clave y nonce con valores de prueba.
 *
 * Clave:  bytes 0x00, 0x01, 0x02, ..., 0x1F  (32 bytes)
 * Nonce:  bytes 0x64, 0x65, 0x66, ..., 0x6F  (12 bytes = valores 100..111)
 *
 * En un sistema real, la clave sería aleatoria (CSPRNG) y el nonce no se
 * reutilizaría nunca. Para efectos demostrativos, estos valores fijos funcionan.
 */
static void fill_test_key_nonce(uint8_t key[CHACHA20_KEY_SIZE], uint8_t nonce[CHACHA20_NONCE_SIZE]) {

    /* Clave: secuencia creciente de bytes 0..31                               */
    for (int i = 0; i < CHACHA20_KEY_SIZE; i++) {
        key[i] = (uint8_t)i;
    }

    /* Nonce: secuencia creciente comenzando en 100 (0x64)                     */
    for (int i = 0; i < CHACHA20_NONCE_SIZE; i++) {
        nonce[i] = (uint8_t)(i + 100);
    }
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * display_histogram_on_lcd() — muestra el histograma HSV en la pantalla ST7735.
 *
 * Delega completamente en la biblioteca libchromascope_lcd:
 *   1. chromascope_lcd_init():  abre /dev/chromascope y envía la secuencia de
 *                               encendido del ST7735 (tablas rcmd1/2/3).
 *   2. lcd_show_histogram():    dibuja barras verticales de colores HSV.
 *   3. chromascope_lcd_deinit(): cierra el file descriptor del driver.
 *
 * No contiene ningún ioctl, open() ni close() directo — toda la interacción
 * con el hardware está encapsulada en libchromascope_lcd.a.
 */
static int display_histogram_on_lcd(const uint32_t *histogram, int bins) {

    int ret = 0;

    /* Abrir el dispositivo e inicializar el display ST7735 via la biblioteca. */
    if (chromascope_lcd_init() < 0) {
        fprintf(stderr, "[Rank 0] No se pudo inicializar la LCD\n");
        return -1;
    }

    /* Dibujar el histograma HSV como barras verticales de colores.
     * La biblioteca limita n_bins a LCD_MAX_BINS (36) internamente.          */
    if (lcd_show_histogram(histogram, (uint32_t)bins) < 0) {
        fprintf(stderr, "[Rank 0] lcd_show_histogram falló\n");
        ret = -1;
    }

    /* Cerrar el dispositivo independientemente del resultado del histograma.  */
    chromascope_lcd_deinit();
    return ret;
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * gpio_completion_sweep() — mueve el servo SG90 como señal visual de que
 * el procesamiento distribuido terminó exitosamente.
 *
 * Secuencia de movimiento (3 pasos):
 *   1. 0°  por 400 ms → posición inicial
 *   2. esperar 500 ms que el servo llegue físicamente
 *   3. 90° por 800 ms → posición media
 *   4. esperar 800 ms
 *   5. 0°  por 800 ms → regresar al origen
 *
 * Delega completamente en libchromascope_gpio.a:
 *   chromascope_gpio_init():  abre /dev/chromascope_gpio
 *   gpio_servo_move():        envía IOCTL_GPIO_SET_SERVO_ANGLE (genera PWM en kernel)
 *   chromascope_gpio_deinit(): cierra el fd del driver
 *
 * No contiene ningún ioctl, open() ni close() directo.
 */
static int gpio_completion_sweep(void) {
    const int servo_pin = 18;  /* pin BCM 18 — señal de control del SG90       */
    int ret = 0;

    /* Abrir el dispositivo GPIO via la biblioteca. */
    if (chromascope_gpio_init() < 0) {
        fprintf(stderr, "[Rank 0] No se pudo inicializar el driver GPIO\n");
        return -1;
    }

    /* ── Movimiento 1: ir a 0° (400 ms de pulsos PWM) ── */
    if (gpio_servo_move(servo_pin, 0, 400) < 0) {
        fprintf(stderr, "[Rank 0] gpio_servo_move(0°) falló\n");
        ret = -1;
        goto done;
    }
    /* Esperar a que el motor llegue físicamente a la posición destino.        */
    usleep(500000);   /* 500 ms                                                  */

    /* ── Movimiento 2: ir a 90° (800 ms de pulsos PWM) ── */
    if (gpio_servo_move(servo_pin, 90, 800) < 0) {
        fprintf(stderr, "[Rank 0] gpio_servo_move(90°) falló\n");
        ret = -1;
        goto done;
    }
    usleep(800000);   /* 800 ms                                                  */

    /* ── Movimiento 3: regresar a 0° ── */
    if (gpio_servo_move(servo_pin, 0, 800) < 0) {
        fprintf(stderr, "[Rank 0] gpio_servo_move(0° retorno) falló\n");
        ret = -1;
    }

done:
    /* Cerrar el dispositivo GPIO independientemente del resultado.            */
    chromascope_gpio_deinit();
    return ret;
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * write_binary_file() — escribe un buffer de bytes en un archivo binario.
 *
 * Se usa para guardar los píxeles cifrados en disco para análisis posterior.
 * El archivo resultante no es un PPM válido (no tiene cabecera), son bytes crudos.
 */
static int write_binary_file(const char *path, const uint8_t *buffer, size_t size) {

    /* Crear o truncar el archivo en modo binario de escritura.                 */
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: no se pudo crear archivo: %s\n", path);
        return -1;
    }

    /* Escribir todos los bytes del buffer en el archivo.                       */
    size_t written = fwrite(buffer, 1, size, file);
    fclose(file);

    /* Verificar que se escribieron exactamente los bytes solicitados.          */
    return written == size ? 0 : -1;
}
