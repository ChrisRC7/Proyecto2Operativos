#include "chacha20.h"

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define CHACHA20_ROUNDS 20

static uint32_t rotl32(uint32_t value, int shift);
static uint32_t load32_le(const uint8_t bytes[4]);
static void store32_le(uint8_t bytes[4], uint32_t value);
static void quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d);

void chacha20_block(
    const uint8_t key[CHACHA20_KEY_SIZE],
    uint32_t counter,
    const uint8_t nonce[CHACHA20_NONCE_SIZE],
    uint8_t output[CHACHA20_BLOCK_SIZE]
) {
    uint32_t state[16];
    uint32_t working_state[16];

    state[0] = 0x61707865;
    state[1] = 0x3320646e;
    state[2] = 0x79622d32;
    state[3] = 0x6b206574;

    state[4] = load32_le(key + 0);
    state[5] = load32_le(key + 4);
    state[6] = load32_le(key + 8);
    state[7] = load32_le(key + 12);
    state[8] = load32_le(key + 16);
    state[9] = load32_le(key + 20);
    state[10] = load32_le(key + 24);
    state[11] = load32_le(key + 28);

    state[12] = counter;

    state[13] = load32_le(nonce + 0);
    state[14] = load32_le(nonce + 4);
    state[15] = load32_le(nonce + 8);

    memcpy(working_state, state, sizeof(state));

    for (int i = 0; i < CHACHA20_ROUNDS; i += 2) {
        quarter_round(&working_state[0], &working_state[4], &working_state[8],  &working_state[12]);
        quarter_round(&working_state[1], &working_state[5], &working_state[9],  &working_state[13]);
        quarter_round(&working_state[2], &working_state[6], &working_state[10], &working_state[14]);
        quarter_round(&working_state[3], &working_state[7], &working_state[11], &working_state[15]);

        quarter_round(&working_state[0], &working_state[5], &working_state[10], &working_state[15]);
        quarter_round(&working_state[1], &working_state[6], &working_state[11], &working_state[12]);
        quarter_round(&working_state[2], &working_state[7], &working_state[8],  &working_state[13]);
        quarter_round(&working_state[3], &working_state[4], &working_state[9],  &working_state[14]);
    }

    for (int i = 0; i < 16; i++) {
        working_state[i] += state[i];
        store32_le(output + (i * 4), working_state[i]);
    }
}

void chacha20_encrypt_buffer(
    const uint8_t key[CHACHA20_KEY_SIZE],
    const uint8_t nonce[CHACHA20_NONCE_SIZE],
    uint32_t counter,
    const uint8_t *input,
    uint8_t *output,
    size_t length
) {
    uint8_t keystream[CHACHA20_BLOCK_SIZE];
    size_t offset = 0;

    while (offset < length) {
        chacha20_block(key, counter, nonce, keystream);

        size_t remaining = length - offset;
        size_t block_size = remaining < CHACHA20_BLOCK_SIZE
                            ? remaining
                            : CHACHA20_BLOCK_SIZE;

        for (size_t i = 0; i < block_size; i++) {
            output[offset + i] = input[offset + i] ^ keystream[i];
        }

        offset += block_size;
        counter++;
    }

    memset(keystream, 0, sizeof(keystream));
}

static void quarter_round(uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    *a += *b;
    *d ^= *a;
    *d = rotl32(*d, 16);

    *c += *d;
    *b ^= *c;
    *b = rotl32(*b, 12);

    *a += *b;
    *d ^= *a;
    *d = rotl32(*d, 8);

    *c += *d;
    *b ^= *c;
    *b = rotl32(*b, 7);
}

static uint32_t rotl32(uint32_t value, int shift) {
    return (value << shift) | (value >> (32 - shift));
}

static uint32_t load32_le(const uint8_t bytes[4]) {
    return ((uint32_t)bytes[0]) |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void store32_le(uint8_t bytes[4], uint32_t value) {
    bytes[0] = (uint8_t)(value);
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
}