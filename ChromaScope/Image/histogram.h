#ifndef HISTOGRAM_H
#define HISTOGRAM_H

#include <stdint.h>

void hsv_histogram_rgb_buffer(
    const uint8_t *pixels,
    int width,
    int height,
    int bins,
    uint32_t *histogram
);

int histogram_find_dominant_bin(const uint32_t *histogram, int bins);

void histogram_print(const uint32_t *histogram, int bins);

#endif