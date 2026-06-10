#include "histogram.h"

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define RGB_CHANNELS 3

static float max3(float a, float b, float c);
static float min3(float a, float b, float c);
static float rgb_to_hue_degrees(uint8_t r, uint8_t g, uint8_t b);

void hsv_histogram_rgb_buffer(
    const uint8_t *pixels,
    int width,
    int height,
    int bins,
    uint32_t *histogram
) {
    if (pixels == NULL || histogram == NULL || width <= 0 || height <= 0 || bins <= 0) {
        return;
    }

    memset(histogram, 0, sizeof(uint32_t) * (size_t)bins);

    int total_pixels = width * height;

    for (int i = 0; i < total_pixels; i++) {
        int index = i * RGB_CHANNELS;

        uint8_t r = pixels[index + 0];
        uint8_t g = pixels[index + 1];
        uint8_t b = pixels[index + 2];

        float hue = rgb_to_hue_degrees(r, g, b);

        int bin = (int)((hue / 360.0f) * bins);

        if (bin < 0) {
            bin = 0;
        }

        if (bin >= bins) {
            bin = bins - 1;
        }

        histogram[bin]++;
    }
}

int histogram_find_dominant_bin(const uint32_t *histogram, int bins) {
    if (histogram == NULL || bins <= 0) {
        return -1;
    }

    int dominant_bin = 0;
    uint32_t max_value = histogram[0];

    for (int i = 1; i < bins; i++) {
        if (histogram[i] > max_value) {
            max_value = histogram[i];
            dominant_bin = i;
        }
    }

    return dominant_bin;
}

void histogram_print(const uint32_t *histogram, int bins) {
    if (histogram == NULL || bins <= 0) {
        return;
    }

    for (int i = 0; i < bins; i++) {
        if (histogram[i] > 0) {
            printf("Bin %02d: %u\n", i, histogram[i]);
        }
    }
}

static float rgb_to_hue_degrees(uint8_t r_u8, uint8_t g_u8, uint8_t b_u8) {
    float r = r_u8 / 255.0f;
    float g = g_u8 / 255.0f;
    float b = b_u8 / 255.0f;

    float max_value = max3(r, g, b);
    float min_value = min3(r, g, b);
    float delta = max_value - min_value;

    if (delta == 0.0f) {
        return 0.0f;
    }

    float hue;

    if (max_value == r) {
        hue = 60.0f * ((g - b) / delta);

        if (hue < 0.0f) {
            hue += 360.0f;
        }
    } else if (max_value == g) {
        hue = 60.0f * (((b - r) / delta) + 2.0f);
    } else {
        hue = 60.0f * (((r - g) / delta) + 4.0f);
    }

    if (hue >= 360.0f) {
        hue -= 360.0f;
    }

    return hue;
}

static float max3(float a, float b, float c) {
    float max = a;

    if (b > max) {
        max = b;
    }

    if (c > max) {
        max = c;
    }

    return max;
}

static float min3(float a, float b, float c) {
    float min = a;

    if (b < min) {
        min = b;
    }

    if (c < min) {
        min = c;
    }

    return min;
}