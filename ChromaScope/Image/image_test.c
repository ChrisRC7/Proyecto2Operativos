#include "ppm.h"
#include "histogram.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#define HIST_BINS 36

static void print_usage(const char *program_name);

int main(int argc, char **argv) {
    if (argc != 2) {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const char *input_path = argv[1];
    const char *gray_output_path = "output_gray.ppm";
    const char *copy_output_path = "output_copy.ppm";

    PPMImage image;

    if (ppm_read(input_path, &image) != 0) {
        fprintf(stderr, "Error: no se pudo leer la imagen.\n");
        return EXIT_FAILURE;
    }

    printf("Imagen cargada correctamente.\n");
    printf("Ancho: %d\n", image.width);
    printf("Alto: %d\n", image.height);

    if (ppm_write(copy_output_path, &image) != 0) {
        fprintf(stderr, "Error: no se pudo guardar copia PPM.\n");
        ppm_free(&image);
        return EXIT_FAILURE;
    }

    printf("Copia guardada en: %s\n", copy_output_path);

    if (ppm_write_gray(gray_output_path, &image) != 0) {
        fprintf(stderr, "Error: no se pudo guardar imagen gris.\n");
        ppm_free(&image);
        return EXIT_FAILURE;
    }

    printf("Imagen en escala de grises guardada en: %s\n", gray_output_path);

    uint32_t histogram[HIST_BINS];

    hsv_histogram_rgb_buffer(
        image.pixels,
        image.width,
        image.height,
        HIST_BINS,
        histogram
    );

    printf("\nHistograma HSV:\n");
    histogram_print(histogram, HIST_BINS);

    int dominant_bin = histogram_find_dominant_bin(histogram, HIST_BINS);

    printf("\nBin dominante: %d\n", dominant_bin);
    printf("Rango aproximado de hue: %d° - %d°\n",
           dominant_bin * 10,
           (dominant_bin + 1) * 10);

    ppm_free(&image);

    return EXIT_SUCCESS;
}

static void print_usage(const char *program_name) {
    fprintf(stderr, "Uso:\n");
    fprintf(stderr, "  %s imagen.ppm\n", program_name);
}