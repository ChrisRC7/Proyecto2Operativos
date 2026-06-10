#ifndef RGB_FILTER_H
#define RGB_FILTER_H

#include <stdint.h>

void rgb_filter_buffer(
    const uint8_t *input,
    uint8_t *output,
    int width,
    int height,
    float r_gain,
    float g_gain,
    float b_gain
);

#endif