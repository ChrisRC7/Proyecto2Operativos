#ifndef PPM_H
#define PPM_H

#include <stdint.h>

typedef struct {
    int width;
    int height;
    uint8_t *pixels;   // RGB plano: width * height * 3 bytes
} PPMImage;

int ppm_read(const char *path, PPMImage *image);
int ppm_write(const char *path, const PPMImage *image);
int ppm_write_gray(const char *path, const PPMImage *image);
void ppm_free(PPMImage *image);

#endif