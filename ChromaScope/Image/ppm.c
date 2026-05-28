#include "ppm.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <ctype.h>
#include <string.h>
#include <limits.h>

#define PPM_CHANNELS 3
#define PPM_MAX_COLOR 255

static int read_next_token(FILE *file, char *buffer, size_t buffer_size);
static int checked_image_size(int width, int height, size_t *out_size);

int ppm_read(const char *path, PPMImage *image) {
    if (path == NULL || image == NULL) {
        return -1;
    }

    image->width = 0;
    image->height = 0;
    image->pixels = NULL;

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: no se pudo abrir el archivo PPM: %s\n", path);
        return -1;
    }

    char token[64];

    if (read_next_token(file, token, sizeof(token)) != 0) {
        fprintf(stderr, "Error: no se pudo leer el magic number del PPM.\n");
        fclose(file);
        return -1;
    }

    if (strcmp(token, "P6") != 0) {
        fprintf(stderr, "Error: formato PPM no soportado. Se esperaba P6.\n");
        fclose(file);
        return -1;
    }

    if (read_next_token(file, token, sizeof(token)) != 0) {
        fprintf(stderr, "Error: no se pudo leer el ancho.\n");
        fclose(file);
        return -1;
    }

    int width = atoi(token);

    if (read_next_token(file, token, sizeof(token)) != 0) {
        fprintf(stderr, "Error: no se pudo leer el alto.\n");
        fclose(file);
        return -1;
    }

    int height = atoi(token);

    if (read_next_token(file, token, sizeof(token)) != 0) {
        fprintf(stderr, "Error: no se pudo leer el valor maximo de color.\n");
        fclose(file);
        return -1;
    }

    int max_color = atoi(token);

    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Error: dimensiones invalidas en PPM.\n");
        fclose(file);
        return -1;
    }

    if (max_color != PPM_MAX_COLOR) {
        fprintf(stderr, "Error: solo se soportan PPM P6 con max color 255.\n");
        fclose(file);
        return -1;
    }

    size_t image_size = 0;
    if (checked_image_size(width, height, &image_size) != 0) {
        fprintf(stderr, "Error: tamano de imagen invalido o demasiado grande.\n");
        fclose(file);
        return -1;
    }

    uint8_t *pixels = malloc(image_size);
    if (pixels == NULL) {
        fprintf(stderr, "Error: no se pudo reservar memoria para la imagen.\n");
        fclose(file);
        return -1;
    }

    size_t bytes_read = fread(pixels, 1, image_size, file);
    fclose(file);

    if (bytes_read != image_size) {
        fprintf(stderr, "Error: el archivo PPM no contiene suficientes datos de pixeles.\n");
        free(pixels);
        return -1;
    }

    image->width = width;
    image->height = height;
    image->pixels = pixels;

    return 0;
}

int ppm_write(const char *path, const PPMImage *image) {
    if (path == NULL || image == NULL || image->pixels == NULL) {
        return -1;
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: no se pudo crear el archivo PPM: %s\n", path);
        return -1;
    }

    fprintf(file, "P6\n%d %d\n255\n", image->width, image->height);

    size_t image_size = 0;
    if (checked_image_size(image->width, image->height, &image_size) != 0) {
        fclose(file);
        return -1;
    }

    size_t bytes_written = fwrite(image->pixels, 1, image_size, file);
    fclose(file);

    if (bytes_written != image_size) {
        fprintf(stderr, "Error: no se pudieron escribir todos los pixeles.\n");
        return -1;
    }

    return 0;
}

int ppm_write_gray(const char *path, const PPMImage *image) {
    if (path == NULL || image == NULL || image->pixels == NULL) {
        return -1;
    }

    size_t image_size = 0;
    if (checked_image_size(image->width, image->height, &image_size) != 0) {
        return -1;
    }

    PPMImage gray_image;
    gray_image.width = image->width;
    gray_image.height = image->height;
    gray_image.pixels = malloc(image_size);

    if (gray_image.pixels == NULL) {
        fprintf(stderr, "Error: no se pudo reservar memoria para imagen gris.\n");
        return -1;
    }

    int total_pixels = image->width * image->height;

    for (int i = 0; i < total_pixels; i++) {
        int index = i * PPM_CHANNELS;

        uint8_t r = image->pixels[index + 0];
        uint8_t g = image->pixels[index + 1];
        uint8_t b = image->pixels[index + 2];

        uint8_t y = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);

        gray_image.pixels[index + 0] = y;
        gray_image.pixels[index + 1] = y;
        gray_image.pixels[index + 2] = y;
    }

    int result = ppm_write(path, &gray_image);

    free(gray_image.pixels);

    return result;
}

void ppm_free(PPMImage *image) {
    if (image == NULL) {
        return;
    }

    free(image->pixels);
    image->pixels = NULL;
    image->width = 0;
    image->height = 0;
}

static int read_next_token(FILE *file, char *buffer, size_t buffer_size) {
    int c;
    size_t index = 0;

    if (file == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    do {
        c = fgetc(file);

        if (c == '#') {
            while (c != '\n' && c != EOF) {
                c = fgetc(file);
            }
        }
    } while (isspace(c));

    if (c == EOF) {
        return -1;
    }

    while (c != EOF && !isspace(c)) {
        if (index + 1 >= buffer_size) {
            return -1;
        }

        buffer[index++] = (char)c;
        c = fgetc(file);
    }

    buffer[index] = '\0';

    return 0;
}

static int checked_image_size(int width, int height, size_t *out_size) {
    if (width <= 0 || height <= 0 || out_size == NULL) {
        return -1;
    }

    size_t w = (size_t)width;
    size_t h = (size_t)height;

    if (w > SIZE_MAX / h) {
        return -1;
    }

    size_t pixels = w * h;

    if (pixels > SIZE_MAX / PPM_CHANNELS) {
        return -1;
    }

    *out_size = pixels * PPM_CHANNELS;

    return 0;
}