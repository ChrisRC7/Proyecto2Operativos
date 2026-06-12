/*
 * test_lcd.c — Programa de prueba que usa libchromascope_lcd.a para el ST7735.
 *
 * Ejecuta una secuencia de pruebas visuales sobre la pantalla LCD TFT 160×128:
 *   1. Inicializar el display (secuencias de configuración del ST7735).
 *   2. Colores primarios: rojo, verde, azul, blanco, negro.
 *   3. Cuatro rectángulos de colores en los cuadrantes de la pantalla.
 *   4. Espectro de colores (rueda HSV completa como franja horizontal).
 *   5. Histograma HSV simulado con distribución tipo campana (bin dominante: verde).
 *   6. Apagar la pantalla (llenar de negro).
 *
 * IMPORTANTE: Este programa ya NO llama a ioctl() directamente.
 * Toda la interacción con el driver chromascope_lcd.ko se realiza a través de
 * libchromascope_lcd.a, la única interfaz autorizada con el hardware LCD.
 *
 * Requiere que el driver esté cargado:
 *   sudo insmod chromascope_lcd.ko
 *
 * Compilar:
 *   gcc -Wall -std=c11 -I../lib test_lcd.c -L../lib -lchromascope_lcd -o test_lcd
 *
 * Ejecutar:
 *   sudo ./test_lcd
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include <stdio.h>    /* printf, fprintf: salida a consola                       */
#include <stdlib.h>   /* EXIT_FAILURE, EXIT_SUCCESS: códigos de retorno          */
#include <stdint.h>   /* uint16_t, uint32_t: tipos de ancho fijo                 */
#include <unistd.h>   /* sleep(): pausas entre pruebas visuales                  */

/*
 * Incluir la biblioteca de usuario LCD: provee chromascope_lcd_init/deinit,
 * lcd_fill_screen, lcd_draw_rect, lcd_show_histogram, y re-exporta todas las
 * constantes de color (LCD_RED, LCD_BLACK...) y el macro RGB565.
 * NO se incluye sys/ioctl.h ni el driver header directamente.
 */
#include "../lib/libchromascope_lcd.h"

/* ── Helpers de prueba ───────────────────────────────────────────────────── */

/*
 * check() — verifica el resultado de una función de biblioteca y termina si falla.
 *
 * Parámetros:
 *   label — nombre de la operación (para el mensaje de error)
 *   ret   — valor retornado por la función de la biblioteca (< 0 indica error)
 */
static void check(const char *label, int ret)
{
    if (ret < 0) {
        /* La biblioteca ya imprimió el motivo del error; aquí solo abortamos. */
        fprintf(stderr, "Error en: %s\n", label);
        /* chromascope_lcd_deinit() cierra el fd si estaba abierto.             */
        chromascope_lcd_deinit();
        exit(EXIT_FAILURE);
    }
}

/* ── Programa principal ─────────────────────────────────────────────────── */

int main(void)
{
    int i;   /* variable de bucle reutilizable                                   */

    /* ── Prueba 1: Inicializar el display ST7735 via la biblioteca ── */
    printf("1. Inicializando ST7735 via libchromascope_lcd...\n");

    /*
     * chromascope_lcd_init() realiza dos pasos internamente:
     *   a) open("/dev/chromascope", O_RDWR) — abre el nodo del driver.
     *   b) ioctl(fd, IOCTL_LCD_INIT)        — envía las tablas rcmd1/2/3 y
     *      enciende el panel (puede tardar ~800 ms por los retardos del ST7735).
     *
     * Si falla (driver no cargado, permisos insuficientes), retorna -1.
     */
    check("chromascope_lcd_init", chromascope_lcd_init());
    printf("   OK — display 160x128 encendido (paisaje)\n\n");

    /* ── Prueba 2: Colores primarios — pantalla completa ── */
    printf("2. Prueba de colores primarios...\n");

    /*
     * lcd_fill_screen() llama internamente a ioctl(fd, IOCTL_LCD_FILL_SCREEN, &cmd).
     * El driver baja CS una sola vez para los 160×128=20480 píxeles.
     * Las constantes (LCD_RED, LCD_GREEN, etc.) son uint16_t en formato RGB565.
     */
    check("fill RED",    lcd_fill_screen(LCD_RED));
    printf("   Rojo\n");    sleep(1);

    check("fill GREEN",  lcd_fill_screen(LCD_GREEN));
    printf("   Verde\n");   sleep(1);

    check("fill BLUE",   lcd_fill_screen(LCD_BLUE));
    printf("   Azul\n");    sleep(1);

    check("fill WHITE",  lcd_fill_screen(LCD_WHITE));
    printf("   Blanco\n");  sleep(1);

    check("fill BLACK",  lcd_fill_screen(LCD_BLACK));
    printf("   Negro\n\n");

    /* ── Prueba 3: Cuatro rectángulos de colores (cuadrantes) ── */
    printf("3. Cuatro rectangulos de colores...\n");

    /*
     * lcd_draw_rect() llama a ioctl(fd, IOCTL_LCD_DRAW_RECT, &cmd).
     * Division de la pantalla 160x128 en 4 cuadrantes aproximados (74x58 px c/u).
     * Se dejan 4 px de margen en cada borde y 8 px de separacion entre cuadrantes.
     */
    check("rect sup-izq", lcd_draw_rect(  4,  4, 74, 58, LCD_RED));    /* rojo    */
    check("rect sup-der", lcd_draw_rect( 82,  4, 74, 58, LCD_GREEN));  /* verde   */
    check("rect inf-izq", lcd_draw_rect(  4, 66, 74, 58, LCD_BLUE));   /* azul    */
    check("rect inf-der", lcd_draw_rect( 82, 66, 74, 58, LCD_YELLOW)); /* amarillo*/
    sleep(2);

    /* ── Prueba 4: Espectro de colores (rueda HSV como franja horizontal) ── */
    printf("4. Espectro de colores (rueda HSV)...\n");

    /* Limpiar la pantalla antes del espectro. */
    check("fill BLACK", lcd_fill_screen(LCD_BLACK));

    for (i = 0; i < LCD_WIDTH; i++) {

        /*
         * Mapear la columna i al angulo de matiz correspondiente en [0 deg, 360 deg).
         * Con LCD_WIDTH=160: columna 0 -> 0 deg, columna 80 -> 180 deg, columna 159 -> 358 deg.
         */
        uint32_t hue     = (uint32_t)i * 360u / (uint32_t)LCD_WIDTH;
        uint32_t sector  = hue / 60u;         /* sector de 60 deg (0-5)          */
        uint32_t frac    = hue % 60u;         /* fraccion dentro del sector [0,59]*/
        uint32_t x_comp  = frac * 255u / 60u; /* interpolacion lineal [0,255]    */
        uint32_t r = 0, g = 0, b = 0;
        uint16_t color;

        /*
         * Calcular R, G, B en funcion del sector de la rueda de colores HSV
         * con S=V=1 (colores puros, maxima saturacion y brillo).
         */
        switch (sector) {
        case 0: r = 255;        g = x_comp; b = 0;           break; /* rojo->amarillo  */
        case 1: r = 255-x_comp; g = 255;    b = 0;           break; /* amarillo->verde */
        case 2: r = 0;          g = 255;    b = x_comp;      break; /* verde->cian     */
        case 3: r = 0;          g = 255-x_comp; b = 255;     break; /* cian->azul      */
        case 4: r = x_comp;     g = 0;      b = 255;         break; /* azul->magenta   */
        default: r = 255;       g = 0;      b = 255-x_comp;  break; /* magenta->rojo   */
        }

        /*
         * Empaquetar en RGB565:
         *   Bits 15-11: 5 bits de rojo   -> (r & 0xF8) << 8
         *   Bits 10-5:  6 bits de verde  -> (g & 0xFC) << 3
         *   Bits  4-0:  5 bits de azul   -> b >> 3
         */
        color = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3u));

        /* Dibujar una franja vertical de 1x40 px en la columna i.
         * La franja empieza en y=44 (centrada verticalmente en 128 px).       */
        check("rect espectro", lcd_draw_rect((uint16_t)i, 44, 1, 40, color));
    }
    sleep(2);

    /* ── Prueba 5: Histograma HSV simulado (36 bins) ── */
    printf("5. Histograma HSV simulado (36 bins)...\n");
    {
        /*
         * Generar distribucion tipo campana centrada en bin 12 (aprox 120 deg = verde).
         * Formula: conteo = max(0, 100 - |i - 12| x 9)
         *   - bin 12:       100 (maximo, verde dominante)
         *   - bin 11 y 13:   91
         *   - bin  3 y 21:    1
         *   - bins fuera de |i-12| >= 10: 0
         * Simula una imagen donde el verde es el color dominante.
         */
        uint32_t bins[LCD_MAX_BINS];

        for (i = 0; i < LCD_MAX_BINS; i++) {
            int diff = i - 12;
            if (diff < 0) diff = -diff;   /* valor absoluto del alejamiento al centro */
            bins[i] = (diff < 10) ? (uint32_t)(100 - diff * 9) : 0u;
        }

        /*
         * lcd_show_histogram() llama internamente a ioctl IOCTL_LCD_SHOW_HISTOGRAM.
         * El driver:
         *   1. Limpia la pantalla con negro.
         *   2. Para cada bin con conteo > 0 dibuja una barra vertical con el
         *      color del matiz HSV que representa ese bin.
         *   3. La altura es proporcional al conteo (normalizada al maximo).
         */
        check("show_histogram", lcd_show_histogram(bins, LCD_MAX_BINS));
    }
    sleep(3);

    /* ── Prueba 6: Apagar la pantalla ── */
    printf("6. Pantalla apagada (negro).\n");

    /* Limpiar la pantalla con negro como estado final. */
    check("fill BLACK final", lcd_fill_screen(LCD_BLACK));

    /* ── Cierre de la biblioteca ── */
    /*
     * chromascope_lcd_deinit() cierra el fd del dispositivo internamente.
     * El display mantiene su ultimo contenido (negro) en el GRAM del ST7735.
     */
    chromascope_lcd_deinit();

    printf("\nTest completado — todas las pruebas pasaron.\n");
    return EXIT_SUCCESS;
}
