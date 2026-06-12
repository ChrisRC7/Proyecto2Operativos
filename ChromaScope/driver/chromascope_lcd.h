/* SPDX-License-Identifier: GPL-2.0
 * chromascope_lcd.h — API pública del driver ST7735 TFT LCD 128×160.
 *                     Compartida entre el módulo kernel y el espacio de usuario.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#ifndef CHROMASCOPE_LCD_H
#define CHROMASCOPE_LCD_H

#ifdef __KERNEL__
#  include <linux/ioctl.h>
#  include <linux/types.h>
#else
#  include <sys/ioctl.h>
#  include <stdint.h>
   typedef uint16_t __u16;
   typedef uint32_t __u32;
#endif

/* ── Identificación del driver ───────────────────────────────────────────── */
#define LCD_MAGIC  'L'
#define LCD_DEV    "/dev/chromascope"

/* ── Dimensiones del display ─────────────────────────────────────────────── */
#define LCD_WIDTH   160
#define LCD_HEIGHT  128

/* ── Número máximo de bins del histograma HSV ────────────────────────────── */
#define LCD_MAX_BINS  36

/* ── Colores RGB565 de uso frecuente ─────────────────────────────────────── */
/*    RGB565: RRRRR GGGGGG BBBBB  (5-6-5 bits)                                */
#define LCD_BLACK    ((__u16)0x0000)
#define LCD_WHITE    ((__u16)0xFFFF)
#define LCD_RED      ((__u16)0x001F)
#define LCD_GREEN    ((__u16)0x07E0)
#define LCD_BLUE     ((__u16)0xF800)
#define LCD_YELLOW   ((__u16)0xFFE0)
#define LCD_CYAN     ((__u16)0x07FF)
#define LCD_MAGENTA  ((__u16)0xF81F)
#define LCD_ORANGE   ((__u16)0xFD20)

/* ── Macro para construir colores RGB565 ─────────────────────────────────── */
#define RGB565(r, g, b) \
    ((__u16)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

/* ── Estructuras de argumento para ioctl ─────────────────────────────────── */

/* IOCTL_LCD_FILL_SCREEN — rellena toda la pantalla con un color */
struct lcd_cmd_fill {
    __u16 color;           /* RGB565 */
};

/* IOCTL_LCD_DRAW_PIXEL — dibuja un píxel individual */
struct lcd_cmd_pixel {
    __u16 x;
    __u16 y;
    __u16 color;           /* RGB565 */
};

/* IOCTL_LCD_DRAW_RECT — dibuja un rectángulo relleno */
struct lcd_cmd_rect {
    __u16 x;               /* columna inicial  (0 = izquierda) */
    __u16 y;               /* fila inicial     (0 = arriba)    */
    __u16 w;               /* ancho en píxeles */
    __u16 h;               /* alto  en píxeles */
    __u16 color;           /* RGB565 */
};

/* IOCTL_LCD_SHOW_HISTOGRAM — muestra histograma HSV como barras de colores */
struct lcd_cmd_histogram {
    __u32 bins[LCD_MAX_BINS]; /* conteos por bin (índice i = hue i×360/n_bins°) */
    __u32 n_bins;             /* número de bins activos (máx LCD_MAX_BINS) */
};

/* ── Comandos ioctl ──────────────────────────────────────────────────────── */
/*
 *  IOCTL_LCD_INIT           — inicializa y enciende la pantalla
 *  IOCTL_LCD_FILL_SCREEN    — rellena toda la pantalla con un color
 *  IOCTL_LCD_DRAW_PIXEL     — dibuja un píxel en (x, y)
 *  IOCTL_LCD_DRAW_RECT      — dibuja rectángulo relleno
 *  IOCTL_LCD_SHOW_HISTOGRAM — visualiza histograma HSV (barras de colores)
 */
#define IOCTL_LCD_INIT            _IO (LCD_MAGIC, 1)
#define IOCTL_LCD_FILL_SCREEN     _IOW(LCD_MAGIC, 2, struct lcd_cmd_fill)
#define IOCTL_LCD_DRAW_PIXEL      _IOW(LCD_MAGIC, 3, struct lcd_cmd_pixel)
#define IOCTL_LCD_DRAW_RECT       _IOW(LCD_MAGIC, 4, struct lcd_cmd_rect)
#define IOCTL_LCD_SHOW_HISTOGRAM  _IOW(LCD_MAGIC, 5, struct lcd_cmd_histogram)

#endif /* CHROMASCOPE_LCD_H */
