/* ppm.c — Lectura y escritura de imágenes en formato PPM binario (P6).
 *
 * El formato PPM P6 consiste en:
 *   1. Cabecera de texto: "P6\n<ancho> <alto>\n<max_color>\n"
 *   2. Datos binarios: width × height × 3 bytes (R, G, B por píxel)
 *
 * El parser maneja comentarios (#) y espacios entre tokens de la cabecera.
 * Se incluyen protecciones contra overflow aritmético al calcular tamaños.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include "ppm.h"        /* struct PPMImage y declaraciones públicas             */

#include <stdio.h>      /* FILE, fopen, fclose, fread, fwrite, fgets, fprintf   */
#include <stdlib.h>     /* malloc, free, atoi                                   */
#include <stdint.h>     /* uint8_t: tipo de byte sin signo                      */
#include <ctype.h>      /* isspace: detectar espacios y saltos de línea         */
#include <string.h>     /* strcmp: comparar el magic number "P6"                */
#include <limits.h>     /* SIZE_MAX: máximo valor de size_t (para anti-overflow) */

/* Bytes por píxel RGB (R, G, B): constante interna del módulo.                 */
#define PPM_CHANNELS   3

/* Valor máximo de intensidad que acepta este parser.
 * PPM permite max_color hasta 65535, pero aquí solo soportamos 255 (8 bits).   */
#define PPM_MAX_COLOR  255

/* ── Funciones privadas de este módulo ──────────────────────────────────── */
static int read_next_token(FILE *file, char *buffer, size_t buffer_size);
static int checked_image_size(int width, int height, size_t *out_size);

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * ppm_read() — abre un archivo PPM P6 y carga su contenido en `image`.
 *
 * Devuelve 0 en éxito; -1 en cualquier error (archivo no encontrado,
 * formato incorrecto, memoria insuficiente, datos incompletos).
 */
int ppm_read(const char *path, PPMImage *image) {

    /* Validar punteros de entrada antes de cualquier operación.                 */
    if (path == NULL || image == NULL) {
        return -1;
    }

    /* Inicializar la estructura de salida a un estado "vacío" seguro.
     * Si algo falla después, el llamador no recibirá datos basura.             */
    image->width  = 0;
    image->height = 0;
    image->pixels = NULL;

    /* Abrir el archivo en modo binario ("rb") para leer los datos de píxeles
     * sin conversión de saltos de línea (\r\n → \n en Windows).               */
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: no se pudo abrir el archivo PPM: %s\n", path);
        return -1;
    }

    char token[64];   /* buffer para tokens de la cabecera (magic, ancho, alto) */

    /* ── Leer magic number ("P6") ── */
    if (read_next_token(file, token, sizeof(token)) != 0) {
        fprintf(stderr, "Error: no se pudo leer el magic number del PPM.\n");
        fclose(file);
        return -1;
    }

    /* Verificar que el archivo sea PPM binario P6 (no P3 texto ni otros).      */
    if (strcmp(token, "P6") != 0) {
        fprintf(stderr, "Error: formato PPM no soportado. Se esperaba P6.\n");
        fclose(file);
        return -1;
    }

    /* ── Leer el ancho en píxeles ── */
    if (read_next_token(file, token, sizeof(token)) != 0) {
        fprintf(stderr, "Error: no se pudo leer el ancho.\n");
        fclose(file);
        return -1;
    }
    int width = atoi(token);  /* convertir string "480" → entero 480            */

    /* ── Leer el alto en píxeles ── */
    if (read_next_token(file, token, sizeof(token)) != 0) {
        fprintf(stderr, "Error: no se pudo leer el alto.\n");
        fclose(file);
        return -1;
    }
    int height = atoi(token);

    /* ── Leer el valor máximo de color ── */
    if (read_next_token(file, token, sizeof(token)) != 0) {
        fprintf(stderr, "Error: no se pudo leer el valor maximo de color.\n");
        fclose(file);
        return -1;
    }
    int max_color = atoi(token);

    /* Validar que las dimensiones sean positivas y razonables.                  */
    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Error: dimensiones invalidas en PPM.\n");
        fclose(file);
        return -1;
    }

    /* Solo procesamos imágenes de 8 bits por canal (max_color == 255).         */
    if (max_color != PPM_MAX_COLOR) {
        fprintf(stderr, "Error: solo se soportan PPM P6 con max color 255.\n");
        fclose(file);
        return -1;
    }

    /* ── Calcular el tamaño del buffer de píxeles con verificación de overflow */
    size_t image_size = 0;
    if (checked_image_size(width, height, &image_size) != 0) {
        fprintf(stderr, "Error: tamano de imagen invalido o demasiado grande.\n");
        fclose(file);
        return -1;
    }

    /* ── Reservar memoria para todos los píxeles (width × height × 3 bytes) ── */
    uint8_t *pixels = malloc(image_size);
    if (pixels == NULL) {
        fprintf(stderr, "Error: no se pudo reservar memoria para la imagen.\n");
        fclose(file);
        return -1;
    }

    /* Después del último token de cabecera hay exactamente un byte de espacio
     * (generalmente '\n') que separa la cabecera de los datos binarios.
     * fread comienza justo donde fgetc dejó el cursor del archivo.             */
    size_t bytes_read = fread(pixels, 1, image_size, file);
    fclose(file);   /* liberar el handle de archivo inmediatamente tras leer    */

    /* Verificar que se leyeron todos los bytes esperados.                       */
    if (bytes_read != image_size) {
        fprintf(stderr, "Error: el archivo PPM no contiene suficientes datos de pixeles.\n");
        free(pixels);  /* evitar memory leak si hay error                       */
        return -1;
    }

    /* ── Rellenar la estructura de salida con los datos cargados ── */
    image->width  = width;
    image->height = height;
    image->pixels = pixels;   /* el llamador es responsable de liberarlo con ppm_free() */

    return 0;   /* éxito */
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * ppm_write() — guarda una imagen PPM P6 en disco.
 *
 * Escribe primero la cabecera de texto y luego los datos binarios RGB.
 * Devuelve 0 en éxito, -1 en error.
 */
int ppm_write(const char *path, const PPMImage *image) {

    /* Validar que todos los punteros son válidos antes de tocar archivos.      */
    if (path == NULL || image == NULL || image->pixels == NULL) {
        return -1;
    }

    /* Abrir en modo binario de escritura ("wb"): crea el archivo o lo trunca.  */
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: no se pudo crear el archivo PPM: %s\n", path);
        return -1;
    }

    /* Escribir cabecera de texto: magic "P6", ancho, alto y max_color=255.     */
    fprintf(file, "P6\n%d %d\n255\n", image->width, image->height);

    /* Calcular cuántos bytes de píxeles hay que escribir.                       */
    size_t image_size = 0;
    if (checked_image_size(image->width, image->height, &image_size) != 0) {
        fclose(file);
        return -1;
    }

    /* Escribir todos los bytes de píxeles en formato binario.                   */
    size_t bytes_written = fwrite(image->pixels, 1, image_size, file);
    fclose(file);

    /* Verificar que se escribieron todos los bytes.                             */
    if (bytes_written != image_size) {
        fprintf(stderr, "Error: no se pudieron escribir todos los pixeles.\n");
        return -1;
    }

    return 0;
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * ppm_write_gray() — convierte la imagen a escala de grises y la guarda como PPM.
 *
 * Usa la fórmula de luminancia perceptual (Rec. 601):
 *   Y = 0.299×R + 0.587×G + 0.114×B
 * Los coeficientes reflejan la sensibilidad relativa del ojo humano a cada canal.
 * La imagen de salida sigue siendo RGB (3 canales), pero R=G=B=Y en cada píxel.
 */
int ppm_write_gray(const char *path, const PPMImage *image) {

    if (path == NULL || image == NULL || image->pixels == NULL) {
        return -1;
    }

    /* Calcular tamaño del buffer de salida (mismo que la imagen original).     */
    size_t image_size = 0;
    if (checked_image_size(image->width, image->height, &image_size) != 0) {
        return -1;
    }

    /* Crear una imagen de trabajo de las mismas dimensiones.                    */
    PPMImage gray_image;
    gray_image.width  = image->width;
    gray_image.height = image->height;
    gray_image.pixels = malloc(image_size);

    if (gray_image.pixels == NULL) {
        fprintf(stderr, "Error: no se pudo reservar memoria para imagen gris.\n");
        return -1;
    }

    int total_pixels = image->width * image->height;

    /* Recorrer cada píxel y calcular su luminancia Y.                           */
    for (int i = 0; i < total_pixels; i++) {
        int index = i * PPM_CHANNELS;  /* offset del píxel i en el array lineal */

        uint8_t r = image->pixels[index + 0];  /* componente Rojo               */
        uint8_t g = image->pixels[index + 1];  /* componente Verde              */
        uint8_t b = image->pixels[index + 2];  /* componente Azul               */

        /* Luminancia perceptual: promedio ponderado según sensibilidad del ojo. */
        uint8_t y = (uint8_t)(0.299f * r + 0.587f * g + 0.114f * b);

        /* Asignar Y a los tres canales: crea el efecto gris en una imagen RGB. */
        gray_image.pixels[index + 0] = y;   /* R = Y */
        gray_image.pixels[index + 1] = y;   /* G = Y */
        gray_image.pixels[index + 2] = y;   /* B = Y */
    }

    /* Reutilizar ppm_write para guardar la imagen gris en disco.               */
    int result = ppm_write(path, &gray_image);

    free(gray_image.pixels);   /* liberar buffer temporal                       */

    return result;
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * ppm_free() — libera los recursos de una PPMImage creada por ppm_read().
 *
 * Pone pixels=NULL y dimensiones=0 para que el objeto quede en estado "vacío"
 * y una doble llamada a ppm_free() sea segura (free(NULL) es un no-op).
 */
void ppm_free(PPMImage *image) {
    if (image == NULL) {
        return;
    }

    free(image->pixels);   /* liberar el buffer de píxeles en el heap           */
    image->pixels = NULL;  /* evitar uso del puntero liberado ("dangling pointer")*/
    image->width  = 0;
    image->height = 0;
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * read_next_token() — lee el siguiente token de texto del archivo PPM,
 * saltando espacios en blanco y comentarios (#...\n).
 *
 * Los tokens de la cabecera PPM son: "P6", ancho, alto y max_color.
 * Un comentario puede aparecer entre cualquier par de tokens.
 *
 * Devuelve 0 en éxito, -1 si llega a EOF sin encontrar un token.
 */
static int read_next_token(FILE *file, char *buffer, size_t buffer_size) {
    int    c;
    size_t index = 0;

    if (file == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    /* Bucle: saltar espacios y comentarios hasta encontrar un carácter útil.    */
    do {
        c = fgetc(file);   /* leer un carácter del archivo                      */

        /* Si es '#', avanzar hasta el fin de la línea (comentario PPM).        */
        if (c == '#') {
            while (c != '\n' && c != EOF) {
                c = fgetc(file);
            }
        }

    } while (isspace(c));  /* repetir mientras sea espacio (espacio, tab, \n)   */

    /* Si llegamos al final del archivo sin encontrar nada, reportar error.      */
    if (c == EOF) {
        return -1;
    }

    /* Leer caracteres no-espacio hasta el siguiente separador o EOF.            */
    while (c != EOF && !isspace(c)) {
        if (index + 1 >= buffer_size) {
            return -1;  /* token más largo que el buffer: error de formato       */
        }

        buffer[index++] = (char)c;  /* acumular el carácter en el buffer        */
        c = fgetc(file);            /* avanzar al siguiente carácter             */
    }

    buffer[index] = '\0';   /* terminar el string con null para que sea válido   */

    return 0;
}

/* ────────────────────────────────────────────────────────────────────────── */

/*
 * checked_image_size() — calcula width × height × 3 con protección de overflow.
 *
 * En C, si (width * height) > SIZE_MAX el resultado silenciosamente desborda.
 * Esto podría hacer que malloc() reserve muy poca memoria y luego fread()
 * escriba fuera del buffer (buffer overflow).
 *
 * La protección: verificar que (a × b) ≤ SIZE_MAX comprobando a ≤ SIZE_MAX/b.
 */
static int checked_image_size(int width, int height, size_t *out_size) {

    if (width <= 0 || height <= 0 || out_size == NULL) {
        return -1;
    }

    size_t w = (size_t)width;
    size_t h = (size_t)height;

    /* Verificar que w × h no desborde size_t.                                  */
    if (w > SIZE_MAX / h) {
        return -1;   /* imagen imposiblemente grande                            */
    }

    size_t pixels = w * h;  /* número total de píxeles                         */

    /* Verificar que pixels × PPM_CHANNELS no desborde.                         */
    if (pixels > SIZE_MAX / PPM_CHANNELS) {
        return -1;
    }

    *out_size = pixels * PPM_CHANNELS;  /* escribir el resultado en el puntero  */

    return 0;
}
