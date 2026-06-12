/* chacha20.h — Interfaz pública del cifrador de flujo ChaCha20.
 *
 * ChaCha20 es un cifrador de flujo diseñado por Daniel J. Bernstein (2008).
 * Opera sobre bloques de 64 bytes produciendo un "keystream" seudoaleatorio
 * que se combina con los datos mediante XOR.
 *
 * Características de seguridad relevantes:
 *   - Clave de 256 bits (32 bytes): espacio de claves 2^256.
 *   - Nonce de 96 bits (12 bytes): evita reutilización del keystream.
 *   - Contador de 32 bits: numera cada bloque de 64 bytes.
 *   - 20 rondas de quarter_round: resistencia a criptoanálisis diferencial.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#ifndef CHACHA20_H          /* guarda de inclusión: evita redefiniciones si se */
#define CHACHA20_H          /* incluye este header más de una vez en una unidad */

#include <stdint.h>         /* tipos de ancho fijo: uint8_t, uint32_t, etc.    */
#include <stddef.h>         /* size_t: tipo para tamaños de memoria             */

/* ── Constantes del protocolo ChaCha20 (RFC 8439) ───────────────────────── */

/* Tamaño de la clave secreta en bytes: 256 bits = 32 bytes.
 * Una clave más larga implica mayor resistencia a fuerza bruta.                */
#define CHACHA20_KEY_SIZE    32

/* Tamaño del nonce ("number used once") en bytes: 96 bits = 12 bytes.
 * El nonce debe ser único para cada mensaje cifrado con la misma clave;
 * reutilizarlo rompe la seguridad porque dos mensajes tendrían el mismo XOR.   */
#define CHACHA20_NONCE_SIZE  12

/* Tamaño de un bloque de keystream en bytes: 512 bits = 64 bytes.
 * Cada llamada a chacha20_block() produce exactamente 64 bytes de keystream.
 * Mensajes más largos requieren múltiples bloques (el contador se incrementa).  */
#define CHACHA20_BLOCK_SIZE  64

/* ── Declaraciones de funciones ─────────────────────────────────────────── */

/*
 * chacha20_block() — genera un único bloque de keystream de 64 bytes.
 *
 * Construye el estado inicial de 16 palabras de 32 bits:
 *   [constantes "expand 32-byte k"] [clave 8×32] [contador] [nonce 3×32]
 * y aplica 20 rondas de mezcla (10 rondas de columna + 10 de diagonal).
 * El estado final se suma al inicial (esto hace que la función sea
 * no-invertible sin conocer la clave) y se serializa en `output`.
 *
 * Parámetros:
 *   key     — clave secreta de 32 bytes (256 bits).
 *   counter — número de bloque (0 para el primero, +1 por cada bloque).
 *   nonce   — valor único de 12 bytes; nunca reutilizar con la misma clave.
 *   output  — buffer de 64 bytes donde se escribe el keystream generado.
 */
void chacha20_block(
    const uint8_t key[CHACHA20_KEY_SIZE],
    uint32_t counter,
    const uint8_t nonce[CHACHA20_NONCE_SIZE],
    uint8_t output[CHACHA20_BLOCK_SIZE]
);

/*
 * chacha20_encrypt_buffer() — cifra (o descifra) un buffer de longitud arbitraria.
 *
 * ChaCha20 es simétrico respecto al XOR:
 *   cifrar:   plain  XOR keystream = ciphertext
 *   descifrar: ciphertext XOR keystream = plain
 * Por eso la misma función sirve para ambas operaciones con los mismos parámetros.
 *
 * Internamente genera bloques de 64 bytes y hace XOR byte a byte.
 * El último bloque puede ser parcial (si length no es múltiplo de 64).
 *
 * Parámetros:
 *   key     — clave secreta de 32 bytes.
 *   nonce   — nonce de 12 bytes (único por mensaje+clave).
 *   counter — valor inicial del contador de bloques (normalmente 1).
 *   input   — datos de entrada (texto plano o cifrado).
 *   output  — datos de salida (cifrados o descifrados).
 *   length  — número de bytes a procesar.
 */
void chacha20_encrypt_buffer(
    const uint8_t key[CHACHA20_KEY_SIZE],
    const uint8_t nonce[CHACHA20_NONCE_SIZE],
    uint32_t counter,
    const uint8_t *input,
    uint8_t *output,
    size_t length
);

#endif /* CHACHA20_H */
