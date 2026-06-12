/* mpi_protocol.h — Protocolo de comunicación MPI entre master y workers.
 *
 * Define las constantes de tags, los rangos de procesos y la estructura
 * StripMetadata que se transmite como primer mensaje a cada worker.
 * Todo el estado necesario para procesar una franja (dimensiones, clave
 * criptográfica, parámetros del filtro) viaja en un solo MPI_Send.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#ifndef MPI_PROTOCOL_H      /* guarda de inclusión: evita doble definición      */
#define MPI_PROTOCOL_H

#include <stdint.h>         /* uint8_t, uint32_t: tipos de ancho fijo           */
#include "../Crypto/chacha20.h"  /* CHACHA20_KEY_SIZE y CHACHA20_NONCE_SIZE     */

/* ── Identificadores de proceso ─────────────────────────────────────────── */

/* Rank 0 es siempre el proceso maestro en MPI_COMM_WORLD.
 * Reparte trabajo, recolecta resultados y accede a los periféricos.             */
#define MASTER_RANK         0

/* ── Configuración del clúster ──────────────────────────────────────────── */

/* Número ideal de procesos MPI: 1 master + 3 workers = 4 procesos totales.    */
#define EXPECTED_MPI_PROCESSES  4

/* Mínimo aceptable: al menos 1 master + 1 worker.
 * Con menos de 2 procesos no hay paralelismo real.                              */
#define MIN_MPI_PROCESSES   2

/* Número de workers cuando se usa la configuración completa (4 nodos).         */
#define NUM_WORKERS         3

/* ── Constantes de imagen ────────────────────────────────────────────────── */

/* Canales por píxel en formato RGB sin canal alfa: R, G, B = 3 bytes/píxel.   */
#define CHANNELS_RGB        3

/* Número de bins del histograma HSV: 36 bins × 10°/bin = 360° de matiz.
 * Un bin agrupa todos los píxeles cuyo tono cae en un intervalo de 10°.        */
#define HIST_BINS           36

/* ── Tags de mensaje MPI ─────────────────────────────────────────────────── */

/* Cada MPI_Send/MPI_Recv usa un tag numérico para distinguir el tipo de dato.
 * El receptor puede filtrar mensajes por tag, evitando confundir metadatos
 * con píxeles cifrados, histogramas o franjas filtradas.                        */

/* TAG_METADATA: primer mensaje del master al worker; lleva la struct abajo.    */
#define TAG_METADATA        100

/* TAG_IMAGE_STRIP: segundo mensaje; lleva los bytes RGB cifrados de la franja. */
#define TAG_IMAGE_STRIP     101

/* TAG_HISTOGRAM: (reservado) envío explícito de histograma parcial.
 * En la implementación actual se usa MPI_Reduce en su lugar.                    */
#define TAG_HISTOGRAM       102

/* TAG_SHUTDOWN: (reservado) señal de terminación para el worker.
 * No se usa en la versión actual porque MPI_Finalize sincroniza el apagado.    */
#define TAG_SHUTDOWN        999

/* TAG_FILTERED_STRIP: mensaje del worker al master con la franja ya filtrada.  */
#define TAG_FILTERED_STRIP  103

/* ── Estructura de metadatos de franja ──────────────────────────────────── */

/*
 * StripMetadata — todo lo que necesita un worker para procesar su franja.
 *
 * Se envía como un bloque contiguo de bytes (MPI_BYTE) en un único MPI_Send,
 * lo que simplifica la serialización: no se necesita un tipo MPI derivado.
 *
 * La franja es una sub-región horizontal de la imagen completa:
 *   píxel[start_row * width] … píxel[(start_row + rows) * width - 1]
 */
typedef struct {

    /* ── Geometría de la imagen completa ── */
    int width;          /* ancho total de la imagen en píxeles                   */
    int height;         /* alto total de la imagen en píxeles                    */

    /* ── Geometría de la franja asignada a este worker ── */
    int start_row;      /* primera fila de la franja (relativa a la imagen)      */
    int rows;           /* número de filas que le corresponden a este worker     */

    /* ── Tamaños de buffer ── */
    int channels;       /* bytes por píxel (siempre 3 para RGB)                  */
    int strip_bytes;    /* bytes totales de la franja: width * rows * channels   */

    /* ── Parámetros del histograma ── */
    int hist_bins;      /* número de bins HSV que debe calcular el worker        */

    /* ── Clave criptográfica ChaCha20 (256 bits) ── */
    /* Se envía al worker para que pueda descifrar la franja que recibe.
     * En producción NO se enviaría la clave en claro; aquí es demostrativo.    */
    uint8_t key[CHACHA20_KEY_SIZE];    /* 32 bytes — clave de sesión             */

    /* Nonce específico de este worker: el master XOR-ea el último byte
     * con el rank para que cada franja tenga un keystream diferente.            */
    uint8_t nonce[CHACHA20_NONCE_SIZE]; /* 12 bytes — valor único por franja    */

    /* Valor inicial del contador de bloques ChaCha20.
     * Con counter=1 el primer bloque de 64 bytes del keystream comienza en 1.  */
    uint32_t crypto_counter;

    /* ── Parámetros del filtro RGB ── */
    /* Ganancias por canal: valores > 1 amplifican, valores < 1 atenúan.
     * En la implementación actual el filtro hace inversión de color (255-x),
     * por lo que estos valores se reciben pero no se aplican directamente.      */
    float filter_r_gain;    /* ganancia del canal Rojo   (ej: 1.15 = +15%)      */
    float filter_g_gain;    /* ganancia del canal Verde  (ej: 1.00 = sin cambio) */
    float filter_b_gain;    /* ganancia del canal Azul   (ej: 0.85 = -15%)      */

} StripMetadata;

#endif /* MPI_PROTOCOL_H */
