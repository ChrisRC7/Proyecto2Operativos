/* rgb_filter.c — Filtro de inversión de colores por canal RGB.
 *
 * El filtro aplica la transformación de inversión de colores:
 *   R' = 255 - R
 *   G' = 255 - G
 *   B' = 255 - B
 *
 * Este efecto produce el "negativo fotográfico" de la imagen:
 * los colores claros se vuelven oscuros y viceversa.
 * El rojo se convierte en cian, el verde en magenta, el azul en amarillo.
 *
 * Nota: los parámetros r_gain, g_gain, b_gain se reciben pero no se usan en
 * esta versión; el filtro de inversión los reemplaza funcionalmente.
 * Los gains están pensados para una versión futura de ajuste de contraste.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include "rgb_filter.h"   /* declaración pública de rgb_filter_buffer()         */

#include <stdint.h>       /* uint8_t: tipo de byte sin signo para píxeles       */
#include <stdlib.h>       /* (incluido por compatibilidad, no se usa aquí)      */

/* Bytes por píxel en formato RGB: siempre 3 (R, G, B).                         */
#define RGB_CHANNELS  3

/*
 * rgb_filter_buffer() — aplica el filtro a un buffer de píxeles RGB en memoria.
 *
 * La franja se procesa in-place mediante dos buffers separados (input/output)
 * para que el master pueda conservar la imagen original sin modificarla.
 *
 * Parámetros:
 *   input   — buffer de entrada con los píxeles originales (solo lectura)
 *   output  — buffer de salida donde se escriben los píxeles filtrados
 *   width   — ancho de la franja en píxeles
 *   height  — alto de la franja en píxeles (número de filas)
 *   r_gain  — ganancia canal rojo   (no utilizada en versión actual)
 *   g_gain  — ganancia canal verde  (no utilizada en versión actual)
 *   b_gain  — ganancia canal azul   (no utilizada en versión actual)
 */
void rgb_filter_buffer(
    const uint8_t *input,
    uint8_t *output,
    int width,
    int height,
    float r_gain,
    float g_gain,
    float b_gain
) {
    /* Suprimir advertencias de "parámetro no utilizado" del compilador.
     * Los gains se reciben por si en el futuro se activa la lógica de ajuste.  */
    (void)r_gain;
    (void)g_gain;
    (void)b_gain;

    /* Validar punteros y dimensiones antes de procesar.                         */
    if (input == NULL || output == NULL || width <= 0 || height <= 0) {
        return;
    }

    int total_pixels = width * height;   /* total de píxeles en la franja       */

    /* Recorrer cada píxel del buffer lineal.                                    */
    for (int i = 0; i < total_pixels; i++) {

        int index = i * RGB_CHANNELS;  /* offset en bytes del píxel i           */

        /* Leer los tres canales del píxel original.                             */
        uint8_t r = input[index + 0];   /* componente Rojo   [0, 255]           */
        uint8_t g = input[index + 1];   /* componente Verde  [0, 255]           */
        uint8_t b = input[index + 2];   /* componente Azul   [0, 255]           */

        /* Aplicar inversión: 255 - x complementa el valor en [0,255].
         * La operación cast a uint8_t es segura porque (255-x) ∈ [0,255].      */
        output[index + 0] = (uint8_t)(255 - r);   /* R' = complemento de R     */
        output[index + 1] = (uint8_t)(255 - g);   /* G' = complemento de G     */
        output[index + 2] = (uint8_t)(255 - b);   /* B' = complemento de B     */
    }
}
