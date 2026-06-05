#include "rgb_filter.h"

#include <stdint.h>
#include <stdlib.h>

#define RGB_CHANNELS 3

void rgb_filter_buffer(
    const uint8_t *input,
    uint8_t *output,
    int width,
    int height,
    float r_gain,
    float g_gain,
    float b_gain
) {
    (void)r_gain;
    (void)g_gain;
    (void)b_gain;

    if (input == NULL || output == NULL || width <= 0 || height <= 0) {
        return;
    }

    int total_pixels = width * height;

    for (int i = 0; i < total_pixels; i++) {
        int index = i * RGB_CHANNELS;

        uint8_t r = input[index + 0];
        uint8_t g = input[index + 1];
        uint8_t b = input[index + 2];

        output[index + 0] = (uint8_t)(255 - r);
        output[index + 1] = (uint8_t)(255 - g);
        output[index + 2] = (uint8_t)(255 - b);
    }
}