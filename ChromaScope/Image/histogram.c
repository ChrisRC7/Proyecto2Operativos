/* histogram.c — Cálculo de histograma de matiz (Hue) en espacio HSV.
 *
 * Un histograma HSV agrupa los píxeles de una imagen según su tono dominante.
 * En lugar de preguntar "qué tan rojo/verde/azul es un píxel", preguntamos
 * "qué color del arco iris le corresponde".
 *
 * Proceso por píxel:
 *   1. RGB [0,255] → RGB normalizado [0.0, 1.0]
 *   2. RGB [0,1]   → Hue [0°, 360°) usando la fórmula hexaconal HSV
 *   3. Hue         → bin: floor(hue / (360 / bins))
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include "histogram.h"  /* declaraciones públicas de este módulo               */

#include <stdio.h>      /* printf: para histogram_print                        */
#include <stdint.h>     /* uint8_t, uint32_t: tipos de ancho fijo              */
#include <string.h>     /* memset: inicializar el array del histograma a cero  */

/* Número de canales por píxel en formato RGB.                                   */
#define RGB_CHANNELS  3

/* ── Funciones auxiliares privadas ──────────────────────────────────────── */
static float max3(float a, float b, float c);
static float min3(float a, float b, float c);
static float rgb_to_hue_degrees(uint8_t r, uint8_t g, uint8_t b);

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * hsv_histogram_rgb_buffer() — construye el histograma de matiz HSV de un
 * buffer de píxeles RGB.
 *
 * Parámetros:
 *   pixels    — array lineal de bytes: R0,G0,B0, R1,G1,B1, ...
 *   width     — ancho de la región de imagen en píxeles
 *   height    — alto de la región de imagen en píxeles
 *   bins      — número de divisiones del círculo de colores (ej: 36 = 10°/bin)
 *   histogram — array de salida de tamaño `bins`; se inicializa a 0 internamente
 */
void hsv_histogram_rgb_buffer(
    const uint8_t *pixels,
    int width,
    int height,
    int bins,
    uint32_t *histogram
) {
    /* Validar que todos los parámetros sean coherentes antes de procesar.       */
    if (pixels == NULL || histogram == NULL || width <= 0 || height <= 0 || bins <= 0) {
        return;
    }

    /* Poner todos los contadores del histograma en cero antes de acumular.     */
    memset(histogram, 0, sizeof(uint32_t) * (size_t)bins);

    int total_pixels = width * height;  /* número total de píxeles en la franja*/

    /* Recorrer cada píxel del buffer lineal.                                    */
    for (int i = 0; i < total_pixels; i++) {

        int index = i * RGB_CHANNELS;  /* posición de byte del píxel i en el array */

        /* Extraer los tres canales del píxel i.                                 */
        uint8_t r = pixels[index + 0];   /* canal Rojo   [0, 255]               */
        uint8_t g = pixels[index + 1];   /* canal Verde  [0, 255]               */
        uint8_t b = pixels[index + 2];   /* canal Azul   [0, 255]               */

        /* Convertir RGB al ángulo de matiz HSV en grados [0.0°, 360.0°).       */
        float hue = rgb_to_hue_degrees(r, g, b);

        /* Mapear el ángulo al índice de bin: bin = floor(hue / (360/bins)).
         * Ejemplo con 36 bins: hue=45° → bin = floor(45 / 10) = 4              */
        int bin = (int)((hue / 360.0f) * bins);

        /* Clampear: hue puede ser exactamente 360 y dar bin == bins.            */
        if (bin < 0)    bin = 0;
        if (bin >= bins) bin = bins - 1;

        /* Incrementar el conteo del bin correspondiente.                        */
        histogram[bin]++;
    }
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * histogram_find_dominant_bin() — devuelve el índice del bin con mayor conteo.
 *
 * El bin dominante representa el rango de matiz más frecuente en la imagen.
 * Con 36 bins y bin=4, el color dominante está entre 40° y 50° (amarillo-verde).
 */
int histogram_find_dominant_bin(const uint32_t *histogram, int bins) {
    if (histogram == NULL || bins <= 0) {
        return -1;   /* error: parámetros inválidos                              */
    }

    int dominant_bin      = 0;             /* índice del bin ganador             */
    uint32_t max_value    = histogram[0];  /* conteo máximo encontrado           */

    /* Recorrer todos los bins buscando el de mayor conteo.                      */
    for (int i = 1; i < bins; i++) {
        if (histogram[i] > max_value) {
            max_value    = histogram[i];  /* nuevo máximo                        */
            dominant_bin = i;             /* guardar su índice                   */
        }
    }

    return dominant_bin;
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * histogram_print() — imprime por consola los bins no-vacíos del histograma.
 *
 * Útil para depuración y para la validación del procesamiento distribuido.
 * Solo imprime bins con conteo > 0 para no generar ruido innecesario.
 */
void histogram_print(const uint32_t *histogram, int bins) {
    if (histogram == NULL || bins <= 0) {
        return;
    }

    for (int i = 0; i < bins; i++) {
        if (histogram[i] > 0) {
            /* "Bin %02d" usa ancho mínimo 2 para alinear la tabla visualmente. */
            printf("Bin %02d: %u\n", i, histogram[i]);
        }
    }
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * rgb_to_hue_degrees() — convierte un píxel RGB a su ángulo de matiz en HSV.
 *
 * Algoritmo (rueda hexaconal de colores):
 *
 *   1. Normalizar: r,g,b ∈ [0,1]
 *   2. max = max(r,g,b),  min = min(r,g,b),  delta = max - min
 *   3. Si delta == 0: píxel acromático (gris), hue = 0° (convención)
 *   4. Si max == r: hue = 60 × ((g-b)/delta)       → sector Rojo-Amarillo-Verde
 *      Si max == g: hue = 60 × ((b-r)/delta + 2)   → sector Verde-Cian-Azul
 *      Si max == b: hue = 60 × ((r-g)/delta + 4)   → sector Azul-Magenta-Rojo
 *   5. Si hue < 0: hue += 360  (mapear al rango positivo)
 */
static float rgb_to_hue_degrees(uint8_t r_u8, uint8_t g_u8, uint8_t b_u8) {

    /* Normalizar de [0,255] a [0.0, 1.0] para la fórmula HSV.                 */
    float r = r_u8 / 255.0f;
    float g = g_u8 / 255.0f;
    float b = b_u8 / 255.0f;

    float max_value = max3(r, g, b);   /* componente más intensa del píxel     */
    float min_value = min3(r, g, b);   /* componente menos intensa             */
    float delta     = max_value - min_value;  /* "extensión" del color          */

    /* Si delta == 0, todos los canales son iguales → píxel gris → sin matiz.   */
    if (delta == 0.0f) {
        return 0.0f;   /* hue indefinido; se asigna 0° por convención           */
    }

    float hue;

    /* Determinar en qué sextante de la rueda cromática cae el píxel.           */
    if (max_value == r) {
        /* Rojo es dominante: hue está entre -60° y +60° (sector rojo).         */
        hue = 60.0f * ((g - b) / delta);

        /* Si g < b, el ángulo es negativo; rotar 360° para mantener [0,360).   */
        if (hue < 0.0f) {
            hue += 360.0f;
        }
    } else if (max_value == g) {
        /* Verde es dominante: hue está entre 60° y 180° (sector verde-cian).   */
        hue = 60.0f * (((b - r) / delta) + 2.0f);
    } else {
        /* Azul es dominante: hue está entre 180° y 300° (sector azul-magenta). */
        hue = 60.0f * (((r - g) / delta) + 4.0f);
    }

    /* Garantizar que el resultado está en [0°, 360°).                          */
    if (hue >= 360.0f) {
        hue -= 360.0f;
    }

    return hue;
}

/* ────────────────────────────────────────────────────────────────────────── */

/* max3() — devuelve el mayor de tres flotantes. */
static float max3(float a, float b, float c) {
    float max = a;
    if (b > max) max = b;
    if (c > max) max = c;
    return max;
}

/* min3() — devuelve el menor de tres flotantes. */
static float min3(float a, float b, float c) {
    float min = a;
    if (b < min) min = b;
    if (c < min) min = c;
    return min;
}
