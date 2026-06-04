#ifndef CHACHA20_H
#define CHACHA20_H

#include <stdint.h>
#include <stddef.h>

#define CHACHA20_KEY_SIZE 32
#define CHACHA20_NONCE_SIZE 12
#define CHACHA20_BLOCK_SIZE 64

void chacha20_block(
    const uint8_t key[CHACHA20_KEY_SIZE],
    uint32_t counter,
    const uint8_t nonce[CHACHA20_NONCE_SIZE],
    uint8_t output[CHACHA20_BLOCK_SIZE]
);

void chacha20_encrypt_buffer(
    const uint8_t key[CHACHA20_KEY_SIZE],
    const uint8_t nonce[CHACHA20_NONCE_SIZE],
    uint32_t counter,
    const uint8_t *input,
    uint8_t *output,
    size_t length
);

#endif