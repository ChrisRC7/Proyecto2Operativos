#ifndef MPI_PROTOCOL_H
#define MPI_PROTOCOL_H

#include <stdint.h>
#include "../Crypto/chacha20.h"
#include "../Crypto/chacha20.h"

#define MASTER_RANK 0

#define EXPECTED_MPI_PROCESSES 4
#define MIN_MPI_PROCESSES 2
#define NUM_WORKERS 3

#define CHANNELS_RGB 3
#define HIST_BINS 36

#define TAG_METADATA 100
#define TAG_IMAGE_STRIP 101
#define TAG_HISTOGRAM 102
#define TAG_SHUTDOWN 999
#define TAG_FILTERED_STRIP 103

typedef struct {
    int width;
    int height;
    int start_row;
    int rows;
    int channels;
    int strip_bytes;
    int hist_bins;

    uint8_t key[CHACHA20_KEY_SIZE];
    uint8_t nonce[CHACHA20_NONCE_SIZE];
    uint32_t crypto_counter;

    float filter_r_gain;
    float filter_g_gain;
    float filter_b_gain;
} StripMetadata;

#endif