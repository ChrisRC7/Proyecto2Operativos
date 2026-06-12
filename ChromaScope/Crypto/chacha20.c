/* chacha20.c — Implementación del cifrador de flujo ChaCha20 (RFC 8439).
 *
 * ChaCha20 es un cifrador de flujo ARX (Add-Rotate-XOR) que produce un
 * "keystream" seudoaleatorio combinando:
 *   - 4 palabras de constante fija ("expand 32-byte k" en ASCII)
 *   - 8 palabras de clave (256 bits)
 *   - 1 palabra de contador de bloque (32 bits)
 *   - 3 palabras de nonce (96 bits)
 * Total: 16 palabras de 32 bits = 64 bytes de estado.
 *
 * El estado pasa por 20 rondas de transformaciones quarter_round
 * (10 rondas de columnas + 10 de diagonales), se suma al estado inicial
 * y se serializa en little-endian como keystream de 64 bytes.
 *
 * El cifrado/descifrado es idéntico: datos XOR keystream.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include "chacha20.h"   /* constantes de tamaño y declaraciones de funciones    */

#include <stdint.h>     /* uint8_t, uint32_t: tipos de ancho fijo exacto        */
#include <stddef.h>     /* size_t: tipo para contar bytes de memoria            */
#include <string.h>     /* memcpy, memset: copias y limpiezas de memoria        */

/* Número de rondas del algoritmo: 20 (estándar ChaCha20).
 * Cada "ronda doble" aplica 4 quarter_rounds en columnas y 4 en diagonales.
 * El bucle itera de 2 en 2 para procesar pares (columna + diagonal) juntos.    */
#define CHACHA20_ROUNDS  20

/* ── Funciones auxiliares privadas (solo visibles en este archivo) ──────── */
static uint32_t rotl32(uint32_t value, int shift);
static uint32_t load32_le(const uint8_t bytes[4]);
static void     store32_le(uint8_t bytes[4], uint32_t value);
static void     quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);

/* ────────────────────────────────────────────────────────────────────────── */

void chacha20_block(
    const uint8_t key[CHACHA20_KEY_SIZE],
    uint32_t counter,
    const uint8_t nonce[CHACHA20_NONCE_SIZE],
    uint8_t output[CHACHA20_BLOCK_SIZE]
) {
    uint32_t state[16];          /* estado inicial de 16 palabras de 32 bits    */
    uint32_t working_state[16];  /* copia de trabajo que se mezcla en las rondas*/

    /* ── Inicializar el estado (sección 2.1 del RFC 8439) ── */

    /* Palabras 0–3: constantes mágicas ASCII de "expand 32-byte k".
     * Son fijas en todas las implementaciones de ChaCha20; no varían.
     * "expa" = 0x61707865,  "nd 3" = 0x3320646e,
     * "2-by" = 0x79622d32,  "te k" = 0x6b206574                               */
    state[0]  = 0x61707865;   /* "expa" en little-endian ASCII                  */
    state[1]  = 0x3320646e;   /* "nd 3"                                         */
    state[2]  = 0x79622d32;   /* "2-by"                                         */
    state[3]  = 0x6b206574;   /* "te k"                                         */

    /* Palabras 4–11: clave de 256 bits leída en bloques de 4 bytes LE.
     * load32_le convierte 4 bytes en little-endian a un uint32_t.               */
    state[4]  = load32_le(key + 0);   /* bytes  0–3  de la clave                */
    state[5]  = load32_le(key + 4);   /* bytes  4–7                             */
    state[6]  = load32_le(key + 8);   /* bytes  8–11                            */
    state[7]  = load32_le(key + 12);  /* bytes 12–15                            */
    state[8]  = load32_le(key + 16);  /* bytes 16–19                            */
    state[9]  = load32_le(key + 20);  /* bytes 20–23                            */
    state[10] = load32_le(key + 24);  /* bytes 24–27                            */
    state[11] = load32_le(key + 28);  /* bytes 28–31                            */

    /* Palabra 12: contador de bloque (empieza en 1, aumenta por cada bloque).
     * Permite generar hasta 2^32 × 64 = 256 GiB de keystream con la misma
     * clave+nonce sin repetir ningún bloque.                                    */
    state[12] = counter;

    /* Palabras 13–15: nonce de 96 bits en 3 palabras de 32 bits.
     * El nonce es "number used once": único por mensaje+clave.                  */
    state[13] = load32_le(nonce + 0);  /* bytes 0–3 del nonce                   */
    state[14] = load32_le(nonce + 4);  /* bytes 4–7                             */
    state[15] = load32_le(nonce + 8);  /* bytes 8–11                            */

    /* Copiar estado inicial a working_state para no perderlo.
     * Al finalizar las rondas, working_state se suma a state (paso crucial).   */
    memcpy(working_state, state, sizeof(state));

    /* ── 20 rondas de mezcla: 10 pares (columna + diagonal) ── */
    for (int i = 0; i < CHACHA20_ROUNDS; i += 2) {

        /* Ronda de columnas: mezcla las 4 columnas de la matriz 4×4.
         * Cada columna está formada por los elementos en las posiciones
         * 0,4,8,12 / 1,5,9,13 / 2,6,10,14 / 3,7,11,15                        */
        quarter_round(&working_state[0], &working_state[4], &working_state[8],  &working_state[12]);
        quarter_round(&working_state[1], &working_state[5], &working_state[9],  &working_state[13]);
        quarter_round(&working_state[2], &working_state[6], &working_state[10], &working_state[14]);
        quarter_round(&working_state[3], &working_state[7], &working_state[11], &working_state[15]);

        /* Ronda de diagonales: mezcla las 4 diagonales de la misma matriz.
         * Intercala los índices para difundir los bits entre columnas.          */
        quarter_round(&working_state[0], &working_state[5], &working_state[10], &working_state[15]);
        quarter_round(&working_state[1], &working_state[6], &working_state[11], &working_state[12]);
        quarter_round(&working_state[2], &working_state[7], &working_state[8],  &working_state[13]);
        quarter_round(&working_state[3], &working_state[4], &working_state[9],  &working_state[14]);
    }

    /* ── Suma final: working_state += state (word-by-word) ── */
    /* Sumar el estado inicial al resultado de las rondas previene ataques de
     * inversión: aunque un atacante vea el output, no puede deducir la clave
     * solo invirtiendo las rondas, porque no conoce el estado inicial.          */
    for (int i = 0; i < 16; i++) {
        working_state[i] += state[i];                 /* suma modular 2^32      */
        store32_le(output + (i * 4), working_state[i]); /* serializar en LE     */
    }
}

/* ────────────────────────────────────────────────────────────────────────── */

void chacha20_encrypt_buffer(
    const uint8_t key[CHACHA20_KEY_SIZE],
    const uint8_t nonce[CHACHA20_NONCE_SIZE],
    uint32_t counter,
    const uint8_t *input,
    uint8_t *output,
    size_t length
) {
    uint8_t keystream[CHACHA20_BLOCK_SIZE]; /* bloque de keystream de 64 bytes  */
    size_t  offset = 0;                     /* posición actual en el buffer     */

    /* Procesar el buffer en bloques de 64 bytes hasta agotarlo.                */
    while (offset < length) {

        /* Generar el bloque de keystream para el contador actual.
         * Cada llamada produce 64 bytes independientes del bloque anterior.    */
        chacha20_block(key, counter, nonce, keystream);

        /* Calcular cuántos bytes quedan y elegir el tamaño del bloque actual.
         * El último bloque puede ser parcial si length no es múltiplo de 64.   */
        size_t remaining  = length - offset;
        size_t block_size = remaining < CHACHA20_BLOCK_SIZE
                            ? remaining            /* último bloque parcial     */
                            : CHACHA20_BLOCK_SIZE; /* bloque completo           */

        /* XOR byte a byte: output[i] = input[i] ^ keystream[i].
         * La misma operación cifra y descifra porque XOR es su propia inversa:
         *   (plain XOR ks) XOR ks = plain                                      */
        for (size_t i = 0; i < block_size; i++) {
            output[offset + i] = input[offset + i] ^ keystream[i];
        }

        offset  += block_size; /* avanzar al siguiente bloque en el buffer      */
        counter++;             /* incrementar contador: siguiente bloque ≠ este  */
    }

    /* Limpiar el keystream de la pila para no dejar material criptográfico
     * en memoria — buena práctica de seguridad aunque aquí sea voluntaria.     */
    memset(keystream, 0, sizeof(keystream));
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * quarter_round() — operación primitiva ARX sobre 4 palabras de 32 bits.
 *
 * Secuencia estándar ChaCha20 (8 operaciones: 4 sumas, 4 XOR, 4 rotaciones):
 *
 *   a += b; d ^= a; d <<<= 16;
 *   c += d; b ^= c; b <<<= 12;
 *   a += b; d ^= a; d <<<= 8;
 *   c += d; b ^= c; b <<<= 7;
 *
 * Cada ronda mezcla los 128 bits (4×32) de forma que cada bit de salida
 * depende de todos los bits de entrada — propiedad de "avalancha".
 */
static void quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {

    /* Primera mitad: mezcla a-b-d */
    *a += *b;              /* suma: combina a con b                              */
    *d ^= *a;              /* XOR: difunde cambios de a hacia d                  */
    *d = rotl32(*d, 16);   /* rotación izquierda 16 bits: dispersa bits de d    */

    /* Segunda parte: mezcla c-d-b */
    *c += *d;              /* suma: propaga d hacia c                            */
    *b ^= *c;              /* XOR: difunde c hacia b                             */
    *b = rotl32(*b, 12);   /* rotación izquierda 12 bits                        */

    /* Tercera parte: segunda mezcla a-b-d */
    *a += *b;              /* segunda suma a+b: asegura no-linealidad            */
    *d ^= *a;              /* segundo XOR con a                                  */
    *d = rotl32(*d, 8);    /* rotación izquierda 8 bits                         */

    /* Cuarta parte: segunda mezcla c-d-b */
    *c += *d;              /* segunda suma c+d                                   */
    *b ^= *c;              /* segundo XOR con c                                  */
    *b = rotl32(*b, 7);    /* rotación izquierda 7 bits: la más pequeña, asimétrica*/
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * rotl32() — rota un entero de 32 bits a la izquierda 'shift' posiciones.
 *
 * Equivalente a: (value << shift) | (value >> (32 - shift))
 * El compilador moderno con -O2 lo convierte en una instrucción ROL nativa.
 * No hay comportamiento indefinido porque shift ∈ {7,8,12,16} (todos < 32).
 */
static uint32_t rotl32(uint32_t value, int shift) {
    return (value << shift) | (value >> (32 - shift));
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * load32_le() — lee 4 bytes consecutivos y los interpreta como uint32_t
 * en formato little-endian (el byte menos significativo está primero).
 *
 * Ejemplo: bytes = {0x01, 0x02, 0x03, 0x04}  →  retorna 0x04030201
 *
 * Operación bit a bit:
 *   byte[0] ocupa bits  0–7  (sin desplazamiento)
 *   byte[1] ocupa bits  8–15 (shift << 8)
 *   byte[2] ocupa bits 16–23 (shift << 16)
 *   byte[3] ocupa bits 24–31 (shift << 24)
 */
static uint32_t load32_le(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0])        |  /* byte menos significativo           */
           ((uint32_t)bytes[1] <<  8)  |  /* segundo byte                       */
           ((uint32_t)bytes[2] << 16)  |  /* tercer byte                        */
           ((uint32_t)bytes[3] << 24);    /* byte más significativo             */
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * store32_le() — escribe un uint32_t en 4 bytes consecutivos en little-endian.
 * Es la operación inversa de load32_le.
 *
 * Ejemplo: value = 0x04030201  →  bytes = {0x01, 0x02, 0x03, 0x04}
 */
static void store32_le(uint8_t bytes[4], uint32_t value) {
    bytes[0] = (uint8_t)(value);         /* bits  0–7:  byte menos significativo*/
    bytes[1] = (uint8_t)(value >>  8);   /* bits  8–15: segundo byte            */
    bytes[2] = (uint8_t)(value >> 16);   /* bits 16–23: tercer byte             */
    bytes[3] = (uint8_t)(value >> 24);   /* bits 24–31: byte más significativo  */
}
