#ifndef MPI_PROTOCOL_H
#define MPI_PROTOCOL_H

#include <stdint.h>

#define MASTER_RANK 0

#define EXPECTED_MPI_PROCESSES 4
#define NUM_WORKERS 3

#define CHANNELS_RGB 3
#define HIST_BINS 36

#define TAG_METADATA 100
#define TAG_IMAGE_STRIP 101
#define TAG_HISTOGRAM 102
#define TAG_SHUTDOWN 999

typedef struct {
    int width;
    int height;
    int start_row;
    int rows;
    int channels;
    int strip_bytes;
    int hist_bins;
} StripMetadata;

#endif