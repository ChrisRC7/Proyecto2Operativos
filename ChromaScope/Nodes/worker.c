/* worker.c — Proceso worker del clúster ChromaScope (Rank ≥ 1).
 *
 * Flujo de ejecución del worker:
 *   1. Esperar la metadata (dimensiones, clave, parámetros) del master.
 *   2. Reservar memoria para la franja cifrada, descifrada y filtrada.
 *   3. Recibir la franja de píxeles cifrada por SPI MPI.
 *   4. Descifrar la franja con ChaCha20 (misma función que cifra = XOR).
 *   5. Tomar un snapshot de recursos del nodo (memoria + CPU).
 *   6. Calcular el histograma HSV parcial sobre la imagen ORIGINAL (descifrada).
 *   7. Aplicar el filtro RGB (inversión de colores) para generar la salida.
 *   8. Combinar histogramas parciales con MPI_Reduce (suma distribuida).
 *   9. Enviar la franja filtrada al master con MPI_Send.
 *  10. Liberar toda la memoria reservada.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include <mpi.h>        /* MPI_Recv, MPI_Send, MPI_Reduce, MPI_Abort: API MPI  */
#include <stdio.h>      /* printf, fprintf: salida de consola y errores        */
#include <stdlib.h>     /* malloc, free, EXIT_FAILURE: memoria dinámica        */
#include <stdint.h>     /* uint8_t, uint32_t: tipos de ancho fijo              */
#include <string.h>     /* memset: inicializar buffers a cero                  */

#include "../Server/mpi_protocol.h"   /* StripMetadata, constantes TAG_*, HIST_BINS */
#include "../Crypto/chacha20.h"       /* chacha20_encrypt_buffer()               */
#include "../Image/histogram.h"       /* hsv_histogram_rgb_buffer()              */
#include "../Image/rgb_filter.h"      /* rgb_filter_buffer()                     */

#include "worker.h"             /* declaración pública de run_worker()           */
#include "resource_monitor.h"   /* print_resource_snapshot()                    */

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * run_worker() — función principal del proceso worker.
 *
 * Es llamada por todos los procesos con rank > 0 después de que el master
 * les asignó trabajo mediante MPI_Send. El worker bloquea en MPI_Recv
 * hasta recibir la metadata, luego procesa su franja de forma independiente.
 */
void run_worker(int rank) {

    StripMetadata metadata;   /* estructura con toda la configuración del trabajo */

    printf("[Rank %d] Worker iniciado, esperando metadata...\n", rank);

    /* ── Paso 1: Recibir la metadata del master ── */
    /*
     * MPI_Recv bloqueante: el worker espera hasta que el master envíe
     * el mensaje con TAG_METADATA. La metadata se recibe como MPI_BYTE
     * porque StripMetadata es una struct plana (sin punteros).
     */
    MPI_Recv(
        &metadata,              /* buffer donde guardar los datos recibidos      */
        sizeof(StripMetadata),  /* número de bytes esperados                     */
        MPI_BYTE,               /* tipo de dato: bytes sin interpretar           */
        MASTER_RANK,            /* origen: solo acepta mensajes del master       */
        TAG_METADATA,           /* tag: distingue este mensaje de otros tipos    */
        MPI_COMM_WORLD,         /* comunicador global (todos los procesos)       */
        MPI_STATUS_IGNORE       /* no necesitamos info de estado del mensaje     */
    );

    /* Imprimir la información de la franja asignada para depuración.           */
    printf("[Rank %d] Metadata recibida:\n", rank);
    printf("[Rank %d]   Imagen total: %dx%d\n", rank, metadata.width, metadata.height);
    printf("[Rank %d]   Franja: start_row=%d, rows=%d\n",
           rank, metadata.start_row, metadata.rows);
    printf("[Rank %d]   Bytes esperados: %d\n", rank, metadata.strip_bytes);

    /* ── Paso 2: Reservar tres buffers del tamaño de la franja ── */
    uint8_t *encrypted_strip = malloc((size_t)metadata.strip_bytes); /* cifrada   */
    uint8_t *decrypted_strip = malloc((size_t)metadata.strip_bytes); /* descifrada*/
    uint8_t *filtered_strip  = malloc((size_t)metadata.strip_bytes); /* filtrada  */

    /* Verificar que todos los malloc tuvieron éxito antes de continuar.        */
    if (encrypted_strip == NULL || decrypted_strip == NULL || filtered_strip == NULL) {
        fprintf(stderr, "[Rank %d] Error: no se pudo reservar memoria para las franjas.\n", rank);

        /* Liberar lo que se pudo reservar antes del fallo (free(NULL) es seguro).*/
        free(encrypted_strip);
        free(decrypted_strip);
        free(filtered_strip);

        /* MPI_Abort termina todos los procesos del comunicador con código de error.*/
        MPI_Abort(MPI_COMM_WORLD, EXIT_FAILURE);
    }

    /* ── Paso 3: Recibir la franja de píxeles cifrada ── */
    /*
     * Segundo MPI_Recv del worker: recibe la franja RGB cifrada con ChaCha20.
     * TAG_IMAGE_STRIP distingue este mensaje de los metadatos.
     * Los datos se reciben como MPI_UNSIGNED_CHAR (bytes unsigned de 8 bits).
     */
    MPI_Recv(
        encrypted_strip,           /* buffer destino para los bytes cifrados    */
        metadata.strip_bytes,      /* número exacto de bytes de la franja       */
        MPI_UNSIGNED_CHAR,         /* tipo MPI para uint8_t                     */
        MASTER_RANK,               /* origen: el master                         */
        TAG_IMAGE_STRIP,           /* tag: distingue de metadata y resultados   */
        MPI_COMM_WORLD,
        MPI_STATUS_IGNORE
    );

    printf("[Rank %d] Franja cifrada recibida correctamente.\n", rank);

    /* ── Paso 4: Descifrar la franja con ChaCha20 ── */
    /*
     * ChaCha20 usa XOR: plain XOR keystream = encrypted.
     * Por tanto: encrypted XOR keystream = plain.
     * La MISMA función sirve para cifrar y descifrar.
     * Se usa la misma clave, nonce y counter que usó el master al cifrar.
     * El nonce es único por worker (el master XOR-eó el último byte con el rank).
     */
    chacha20_encrypt_buffer(
        metadata.key,            /* clave de 32 bytes enviada en la metadata    */
        metadata.nonce,          /* nonce de 12 bytes único para este worker    */
        metadata.crypto_counter, /* contador de bloque inicial (normalmente 1)  */
        encrypted_strip,         /* input: bytes cifrados recibidos del master  */
        decrypted_strip,         /* output: píxeles originales recuperados      */
        (size_t)metadata.strip_bytes  /* longitud del buffer a procesar          */
    );

    printf("[Rank %d] Franja descifrada correctamente con ChaCha20.\n", rank);

    /* ── Paso 5: Tomar snapshot de recursos del nodo ── */
    /* Lee /proc/meminfo y /proc/loadavg para mostrar el estado del nodo.       */
    print_resource_snapshot(rank);

    /* ── Paso 6: Calcular el histograma HSV sobre la imagen ORIGINAL ── */
    /*
     * IMPORTANTE: el histograma se calcula sobre decrypted_strip (la imagen
     * original), NO sobre filtered_strip (que es el negativo fotográfico).
     * El histograma de color es una característica de la imagen original.
     * Si lo calculáramos sobre el filtro, los colores estarían invertidos.
     */
    uint32_t partial_histogram[HIST_BINS];
    memset(partial_histogram, 0, sizeof(partial_histogram)); /* inicializar a 0  */

    hsv_histogram_rgb_buffer(
        decrypted_strip,           /* buffer de píxeles originales               */
        metadata.width,            /* ancho de la imagen completa (no la franja) */
        metadata.rows,             /* filas que le corresponden a este worker    */
        metadata.hist_bins,        /* número de bins (36 = 10°/bin)              */
        partial_histogram          /* histograma parcial de salida               */
    );

    printf("[Rank %d] Histograma HSV parcial calculado sobre imagen original.\n", rank);

    /* ── Paso 7: Aplicar el filtro RGB (inversión de colores) ── */
    /*
     * El worker aplica el filtro sobre la imagen original y guarda el resultado
     * en filtered_strip. Este buffer se devolverá al master para reconstruir
     * la imagen completa filtrada.
     */
    rgb_filter_buffer(
        decrypted_strip,           /* imagen original como entrada              */
        filtered_strip,            /* imagen filtrada como salida               */
        metadata.width,            /* ancho de la imagen                        */
        metadata.rows,             /* filas de esta franja                      */
        metadata.filter_r_gain,    /* ganancia canal R (recibida en metadata)   */
        metadata.filter_g_gain,    /* ganancia canal G                          */
        metadata.filter_b_gain     /* ganancia canal B                          */
    );

    printf("[Rank %d] Filtro RGB aplicado sobre la franja.\n", rank);

    /* ── Paso 8: Consolidar histogramas con MPI_Reduce ── */
    /*
     * MPI_Reduce es una operación colectiva: TODOS los procesos deben llamarla.
     * Cada worker contribuye con su partial_histogram.
     * El master (MASTER_RANK) recibe el global_histogram = suma de todos.
     * MPI_SUM es la operación de reducción: global[i] = sum(partial[i] para cada rank).
     *
     * Los workers no usan global_histogram (es el master quien lo procesa),
     * pero igualmente deben declararlo como buffer receptor de MPI_Reduce.
     */
    uint32_t global_histogram[HIST_BINS];
    memset(global_histogram, 0, sizeof(global_histogram));

    MPI_Reduce(
        partial_histogram,   /* buffer de envío: histograma parcial de este worker */
        global_histogram,    /* buffer de recepción: acumulado en el master        */
        metadata.hist_bins,  /* número de elementos a reducir                     */
        MPI_UINT32_T,        /* tipo de dato de cada elemento                     */
        MPI_SUM,             /* operación: suma todos los valores del mismo bin    */
        MASTER_RANK,         /* rank del proceso que recibe el resultado global    */
        MPI_COMM_WORLD       /* comunicador con todos los procesos                 */
    );

    printf("[Rank %d] Histograma parcial enviado mediante MPI_Reduce.\n", rank);

    /* ── Paso 9: Enviar la franja filtrada al master ── */
    /*
     * Después de MPI_Reduce, el worker envía su franja filtrada.
     * El master la recibe en el loop de MPI_Recv usando start_row para
     * colocarla en la posición correcta de la imagen completa.
     */
    MPI_Send(
        filtered_strip,          /* datos a enviar: píxeles filtrados           */
        metadata.strip_bytes,    /* número de bytes                             */
        MPI_UNSIGNED_CHAR,       /* tipo de dato                                */
        MASTER_RANK,             /* destino: el master                          */
        TAG_FILTERED_STRIP,      /* tag para que el master lo distinga          */
        MPI_COMM_WORLD
    );

    printf("[Rank %d] Franja filtrada enviada al maestro.\n", rank);

    /* ── Paso 10: Liberar toda la memoria reservada ── */
    free(encrypted_strip);   /* buffer de píxeles cifrados                      */
    free(decrypted_strip);   /* buffer de píxeles originales (descifrados)      */
    free(filtered_strip);    /* buffer de píxeles filtrados                     */
}
