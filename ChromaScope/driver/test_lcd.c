/*
 * test_lcd.c — Prueba básica del driver chromascope_lcd:
 *              inicializa el ST7735 y verifica los modos de dibujo.
 *
 * Ejecutar: sudo ./test_lcd
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "chromascope_lcd.h"

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static int lcd_fd = -1;

static void check(const char *label, int ret)
{
    if (ret < 0) {
        perror(label);
        if (lcd_fd >= 0)
            close(lcd_fd);
        exit(EXIT_FAILURE);
    }
}

static void fill(uint16_t color)
{
    struct lcd_cmd_fill f = { .color = color };
    check("ioctl FILL_SCREEN", ioctl(lcd_fd, IOCTL_LCD_FILL_SCREEN, &f));
}

static void rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    struct lcd_cmd_rect r = { .x = x, .y = y, .w = w, .h = h, .color = color };
    check("ioctl DRAW_RECT", ioctl(lcd_fd, IOCTL_LCD_DRAW_RECT, &r));
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    int i;

    lcd_fd = open(LCD_DEV, O_RDWR);
    if (lcd_fd < 0) {
        perror("open " LCD_DEV);
        fprintf(stderr, "¿Está cargado el módulo?  →  sudo insmod chromascope_lcd.ko\n");
        return EXIT_FAILURE;
    }

    /* ── 1. Inicializar pantalla ─────────────────────────────────────────── */
    printf("1. Inicializando ST7735...\n");
    check("ioctl INIT", ioctl(lcd_fd, IOCTL_LCD_INIT));
    printf("   OK — display 160x128 encendido (paisaje)\n\n");

    /* ── 2. Prueba de colores primarios ──────────────────────────────────── */
    printf("2. Prueba de colores primarios...\n");
    fill(LCD_RED);    printf("   Rojo\n");    sleep(1);
    fill(LCD_GREEN);  printf("   Verde\n");   sleep(1);
    fill(LCD_BLUE);   printf("   Azul\n");    sleep(1);
    fill(LCD_WHITE);  printf("   Blanco\n");  sleep(1);
    fill(LCD_BLACK);  printf("   Negro\n\n");

    /* ── 3. Cuatro rectángulos de colores ────────────────────────────────── */
    printf("3. Cuatro rectángulos de colores...\n");
    /* Cuadrantes para 160x128: cada uno 74x58 px */
    rect(  4,  4, 74, 58, LCD_RED);
    rect( 82,  4, 74, 58, LCD_GREEN);
    rect(  4, 66, 74, 58, LCD_BLUE);
    rect( 82, 66, 74, 58, LCD_YELLOW);
    sleep(2);

    /* ── 4. Rueda de color (franja horizontal por hue) ───────────────────── */
    printf("4. Espectro de colores (rueda HSV)...\n");
    fill(LCD_BLACK);
    for (i = 0; i < LCD_WIDTH; i++) {
        uint32_t hue  = (uint32_t)i * 360u / (uint32_t)LCD_WIDTH;
        uint16_t color;
        /* HSV→RGB565 inline (mismo algoritmo que el kernel) */
        uint32_t sector = hue / 60u;
        uint32_t frac   = hue % 60u;
        uint32_t x_comp = frac * 255u / 60u;
        uint32_t r = 0, g = 0, b = 0;
        switch (sector) {
        case 0: r = 255; g = x_comp; b = 0;           break;
        case 1: r = 255 - x_comp; g = 255; b = 0;     break;
        case 2: r = 0; g = 255; b = x_comp;            break;
        case 3: r = 0; g = 255 - x_comp; b = 255;     break;
        case 4: r = x_comp; g = 0; b = 255;            break;
        default: r = 255; g = 0; b = 255 - x_comp;    break;
        }
        color = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
        rect((uint16_t)i, 44, 1, 40, color);
    }
    sleep(2);

    /* ── 5. Histograma HSV simulado ──────────────────────────────────────── */
    printf("5. Histograma HSV simulado (36 bins)...\n");
    {
        struct lcd_cmd_histogram hist;
        hist.n_bins = 36;

        /*
         * Distribución tipo campana centrada en el bin 12 (verde ~120°).
         * Simula una imagen donde el verde es el color dominante.
         */
        for (i = 0; i < 36; i++) {
            int diff = i - 12;
            if (diff < 0) diff = -diff;
            hist.bins[i] = (diff < 10) ? (uint32_t)(100 - diff * 9) : 0u;
        }
        check("ioctl SHOW_HISTOGRAM", ioctl(lcd_fd, IOCTL_LCD_SHOW_HISTOGRAM, &hist));
    }
    sleep(3);

    /* ── 6. Limpiar ──────────────────────────────────────────────────────── */
    printf("6. Pantalla apagada (negro).\n");
    fill(LCD_BLACK);

    close(lcd_fd);
    printf("\nTest completado.\n");
    return EXIT_SUCCESS;
}
