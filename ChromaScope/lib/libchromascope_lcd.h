/*
 * libchromascope_lcd.h — Biblioteca de usuario para el driver ST7735 TFT LCD.
 *
 * Esta biblioteca es la ÚNICA capa que interactúa con el driver de kernel
 * chromascope_lcd.ko. Todo acceso al dispositivo /dev/chromascope debe
 * realizarse a través de las funciones definidas aquí, nunca mediante ioctl
 * directo desde el código de aplicación.
 *
 * Propósito del módulo:
 *   Encapsular las llamadas ioctl al driver en funciones de alto nivel con
 *   tipos propios, manejo de errores uniforme y documentación completa.
 *   La pantalla ST7735 es de 160×128 píxeles en formato RGB565 (16 bits/px).
 *
 * Gestión del dispositivo:
 *   La biblioteca mantiene internamente el file descriptor del dispositivo.
 *   El usuario solo llama a chromascope_lcd_init() al inicio (que también
 *   enciende el display) y chromascope_lcd_deinit() al final.
 *
 * Colores:
 *   El tipo lcd_color_t es un alias de uint16_t para el formato RGB565.
 *   Las constantes LCD_RED, LCD_GREEN, etc. están definidas en
 *   ../driver/chromascope_lcd.h y son accesibles incluyendo ESTE header.
 *
 * Uso típico:
 *   if (chromascope_lcd_init() < 0) { ... error ... }
 *   lcd_fill_screen(LCD_BLACK);
 *   lcd_draw_rect(10, 10, 50, 30, LCD_RED);
 *   lcd_show_histogram(bins, 36);
 *   chromascope_lcd_deinit();
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#ifndef LIBCHROMASCOPE_LCD_H
#define LIBCHROMASCOPE_LCD_H

#include <stdint.h>   /* uint16_t, uint32_t: tipos de ancho exacto en espacio de usuario */

/*
 * Re-exportar el header del driver: provee LCD_WIDTH, LCD_HEIGHT, LCD_MAX_BINS,
 * las constantes de color (LCD_RED, LCD_BLACK, ...), el macro RGB565(r,g,b)
 * y el tipo __u16 / __u32 usados en las structs ioctl internas.
 *
 * Al incluir este header, el código cliente automáticamente tiene acceso
 * a todo lo que necesita sin conocer los detalles del driver.
 */
#include "../driver/chromascope_lcd.h"

/*
 * lcd_color_t — tipo semántico para colores RGB565.
 *
 * Alias de uint16_t para dejar claro en las firmas de función que el argumento
 * representa un color en formato RGB565, no un entero genérico.
 * Uso: lcd_color_t color = RGB565(255, 128, 0);
 */
typedef uint16_t lcd_color_t;

/* ── Inicialización y cierre ──────────────────────────────────────────────── */

/*
 * chromascope_lcd_init() — abre el dispositivo e inicializa el display ST7735.
 *
 * Realiza dos operaciones:
 *   1. Abre /dev/chromascope con O_RDWR.
 *   2. Envía IOCTL_LCD_INIT para ejecutar la secuencia de encendido del ST7735
 *      (reset hardware, tablas rcmd1/2/3, gamma, Display-On).
 *
 * Después de esta llamada el display está encendido y listo para recibir
 * comandos de dibujo. La llamada puede tardar ~500 ms por los retardos de
 * inicialización del ST7735 (Sleep-Out necesita 500 ms según el datasheet).
 *
 * Retorna:
 *    0  — éxito, display encendido y operativo
 *   -1  — error al abrir el dispositivo o al enviar IOCTL_LCD_INIT
 */
int chromascope_lcd_init(void);

/*
 * chromascope_lcd_deinit() — cierra el dispositivo y libera el fd interno.
 *
 * El display mantiene su último contenido en el GRAM hasta que se apague
 * o se inicialice de nuevo. Esta función solo libera el recurso del kernel.
 */
void chromascope_lcd_deinit(void);

/* ── Primitivas de dibujo ─────────────────────────────────────────────────── */

/*
 * lcd_fill_screen() — rellena toda la pantalla (160×128 px) con un color.
 *
 * Es la operación más eficiente para limpiar el display: CS se baja una
 * sola vez para los 20480 píxeles = 40960 bytes de una transacción SPI.
 *
 * Parámetros:
 *   color — color RGB565 de relleno (ej: LCD_BLACK para borrar la pantalla)
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error
 */
int lcd_fill_screen(lcd_color_t color);

/*
 * lcd_draw_pixel() — dibuja un único píxel en las coordenadas (x, y).
 *
 * El origen (0, 0) es la esquina superior izquierda de la pantalla.
 * El driver silencia píxeles fuera de los límites (x >= 160 o y >= 128).
 *
 * Parámetros:
 *   x, y  — coordenadas del píxel (x: columna 0–159, y: fila 0–127)
 *   color — color RGB565 del píxel
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error
 */
int lcd_draw_pixel(uint16_t x, uint16_t y, lcd_color_t color);

/*
 * lcd_draw_rect() — dibuja un rectángulo sólido.
 *
 * El rectángulo ocupa las coordenadas [x, x+w-1] × [y, y+h-1].
 * El driver recorta automáticamente los bordes que excedan la pantalla.
 *
 * Parámetros:
 *   x, y  — esquina superior izquierda del rectángulo
 *   w, h  — ancho y alto en píxeles
 *   color — color RGB565 del relleno
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error
 */
int lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  lcd_color_t color);

/*
 * lcd_show_histogram() — dibuja el histograma HSV como barras verticales de colores.
 *
 * Cada barra representa un rango de ángulos de matiz (Hue):
 *   - Posición horizontal: distribuida equitativamente en los 160 px de ancho.
 *   - Altura: proporcional al conteo del bin, normalizada al máximo (la barra
 *             con más conteo llega al tope de la pantalla de 128 px).
 *   - Color: el matiz HSV correspondiente al bin (bin i → i × 360°/n_bins).
 *
 * La pantalla se limpia automáticamente con negro antes de dibujar las barras.
 *
 * Parámetros:
 *   bins   — array de n_bins conteos (uno por bin de matiz)
 *   n_bins — número de bins activos; si > LCD_MAX_BINS (36) se trunca a 36
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error o puntero bins inválido
 */
int lcd_show_histogram(const uint32_t *bins, uint32_t n_bins);

#endif /* LIBCHROMASCOPE_LCD_H */
