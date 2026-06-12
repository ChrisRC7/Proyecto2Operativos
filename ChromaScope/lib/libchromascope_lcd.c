/*
 * libchromascope_lcd.c — Implementación de la biblioteca de usuario LCD ST7735.
 *
 * Cada función pública de esta biblioteca:
 *   1. Verifica que el dispositivo esté abierto (fd_lcd >= 0).
 *   2. Construye la struct ioctl correspondiente con los argumentos dados.
 *   3. Llama a ioctl() sobre /dev/chromascope.
 *   4. Retorna 0 en éxito o -1 en error, con errno disponible para el llamador.
 *
 * El fd interno (fd_lcd) es un detalle de implementación privado.
 * Toda la complejidad del protocolo ioctl queda encapsulada aquí; el código
 * cliente solo conoce las funciones de alto nivel del header.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include <stdio.h>      /* fprintf, stderr: mensajes de diagnóstico              */
#include <string.h>     /* strerror(), memset(): manejo de errores y buffers     */
#include <fcntl.h>      /* open(), O_RDWR: abrir el nodo de dispositivo          */
#include <unistd.h>     /* close(): cerrar el file descriptor                    */
#include <sys/ioctl.h>  /* ioctl(): enviar comandos al driver de kernel          */
#include <errno.h>      /* errno: código de error del último syscall fallido     */

/* Header compartido del driver: structs lcd_cmd_*, IOCTL_LCD_*, constantes LCD_*.
 * Se incluye indirectamente desde libchromascope_lcd.h, pero lo incluimos
 * explícitamente aquí para dejar clara la dependencia en este archivo .c.      */
#include "../driver/chromascope_lcd.h"

/* Header público de esta biblioteca: declara las funciones que implementamos.   */
#include "libchromascope_lcd.h"

/* ── Estado interno de la biblioteca ──────────────────────────────────────── */

/*
 * fd_lcd — file descriptor del dispositivo /dev/chromascope.
 *
 * Valor -1 indica "no inicializado". Todas las funciones de operación
 * verifican fd_lcd >= 0; si está en -1, retornan -1 con errno = ENODEV.
 *
 * Variable estática de módulo: inaccesible desde código externo, garantizando
 * que nadie cierre o duplique el fd accidentalmente.
 */
static int fd_lcd = -1;

/* ── Inicialización y cierre ─────────────────────────────────────────────── */

/*
 * chromascope_lcd_init() — abre el dispositivo e inicializa el display.
 *
 * Paso 1: open() sobre LCD_DEV (/dev/chromascope).
 *   O_RDWR es necesario para poder emitir ioctl de escritura (_IOW).
 *   Falla si chromascope_lcd.ko no está cargado (no existe el nodo /dev).
 *
 * Paso 2: IOCTL_LCD_INIT envía al driver las tres tablas de inicialización:
 *   - rcmd1: reset SW, frame rate, control de potencia, MADCTL (orientación)
 *   - rcmd2: ventana de dirección CASET 0–159, RASET 0–127
 *   - rcmd3: curvas de corrección gamma + Display On
 *   Esta secuencia tarda ~800 ms por los retardos de estabilización del ST7735.
 */
int chromascope_lcd_init(void)
{
    /* Abrir el nodo de dispositivo del driver LCD. */
    fd_lcd = open(LCD_DEV, O_RDWR);
    if (fd_lcd < 0) {
        fprintf(stderr, "[lcd_lib] No se pudo abrir %s: %s\n",
                LCD_DEV, strerror(errno));
        fprintf(stderr, "[lcd_lib] ¿Está cargado chromascope_lcd.ko?\n");
        return -1;
    }

    /* Enviar la secuencia de inicialización del ST7735.
     * IOCTL_LCD_INIT es _IO (sin argumento): solo el número de comando.     */
    if (ioctl(fd_lcd, IOCTL_LCD_INIT) < 0) {
        fprintf(stderr, "[lcd_lib] IOCTL_LCD_INIT falló: %s\n", strerror(errno));
        close(fd_lcd);
        fd_lcd = -1;   /* dejar la biblioteca en estado "no inicializado" */
        return -1;
    }

    return 0;   /* display abierto y encendido */
}

/*
 * chromascope_lcd_deinit() — cierra el dispositivo y limpia el estado interno.
 */
void chromascope_lcd_deinit(void)
{
    if (fd_lcd >= 0) {
        close(fd_lcd);   /* liberar el file descriptor en el kernel */
        fd_lcd = -1;     /* marcar como "no inicializado" */
    }
}

/* ── Macro auxiliar para simplificar verificación de estado ─────────────── */

/*
 * CHECK_OPEN: si fd_lcd < 0 retorna -1 con errno = ENODEV.
 * Evita repetir la misma verificación al inicio de cada función de operación.
 */
#define CHECK_OPEN()                         \
    do {                                     \
        if (fd_lcd < 0) {                    \
            errno = ENODEV;                  \
            return -1;                       \
        }                                    \
    } while (0)

/* ── Implementaciones de las primitivas de dibujo ───────────────────────── */

/*
 * lcd_fill_screen() — rellena toda la pantalla con un color sólido.
 *
 * Construye struct lcd_cmd_fill con el color RGB565 y lo envía con
 * IOCTL_LCD_FILL_SCREEN. El driver baja CS una sola vez para todos los
 * 160×128 = 20480 píxeles (40960 bytes en una transacción SPI).
 */
int lcd_fill_screen(lcd_color_t color)
{
    struct lcd_cmd_fill cmd;   /* struct del ioctl: solo tiene el campo color */

    /* Verificar que el dispositivo está abierto. */
    CHECK_OPEN();

    /* Rellenar la struct con el color solicitado. */
    cmd.color = color;

    /* Enviar el comando al driver. El driver llama a lcd_fill_screen()
     * que a su vez llama a lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color). */
    if (ioctl(fd_lcd, IOCTL_LCD_FILL_SCREEN, &cmd) < 0) {
        fprintf(stderr, "[lcd_lib] lcd_fill_screen(0x%04X) falló: %s\n",
                color, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * lcd_draw_pixel() — dibuja un único píxel en (x, y).
 *
 * El driver define una ventana CASET/RASET de 1×1 y envía el color como
 * dos bytes por SPI. Los píxeles fuera de [0,159]×[0,127] son silenciados
 * internamente por el driver (no se retorna error, el píxel simplemente no
 * se dibuja). Esta biblioteca no añade validación adicional para mantener
 * consistencia con el comportamiento del driver.
 */
int lcd_draw_pixel(uint16_t x, uint16_t y, lcd_color_t color)
{
    struct lcd_cmd_pixel cmd;   /* struct del ioctl: x, y, color */

    CHECK_OPEN();

    /* Rellenar la struct con las coordenadas y el color. */
    cmd.x     = x;
    cmd.y     = y;
    cmd.color = color;

    if (ioctl(fd_lcd, IOCTL_LCD_DRAW_PIXEL, &cmd) < 0) {
        fprintf(stderr, "[lcd_lib] lcd_draw_pixel(%u,%u,0x%04X) falló: %s\n",
                x, y, color, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * lcd_draw_rect() — dibuja un rectángulo sólido.
 *
 * El driver recorta el rectángulo al borde de la pantalla si (x+w) > 160
 * o (y+h) > 128. CS se baja una sola vez para todos los píxeles del rect.
 */
int lcd_draw_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                  lcd_color_t color)
{
    struct lcd_cmd_rect cmd;   /* struct del ioctl: x, y, w, h, color */

    CHECK_OPEN();

    /* Rellenar todos los campos de la struct. */
    cmd.x     = x;
    cmd.y     = y;
    cmd.w     = w;
    cmd.h     = h;
    cmd.color = color;

    if (ioctl(fd_lcd, IOCTL_LCD_DRAW_RECT, &cmd) < 0) {
        fprintf(stderr,
                "[lcd_lib] lcd_draw_rect(%u,%u,%u,%u,0x%04X) falló: %s\n",
                x, y, w, h, color, strerror(errno));
        return -1;
    }

    return 0;
}

/*
 * lcd_show_histogram() — visualiza el histograma HSV como barras de colores.
 *
 * Construye struct lcd_cmd_histogram copiando los n_bins conteos del array
 * bins[] y lo envía con IOCTL_LCD_SHOW_HISTOGRAM.
 *
 * El driver:
 *   1. Limpia la pantalla con negro.
 *   2. Para cada bin i con conteo > 0:
 *      - Calcula el color = matiz i × 360°/n_bins convertido a RGB565.
 *      - Dibuja una barra vertical de altura proporcional al conteo.
 */
int lcd_show_histogram(const uint32_t *bins, uint32_t n_bins)
{
    struct lcd_cmd_histogram cmd;   /* struct del ioctl: bins[], n_bins */
    uint32_t i;                     /* índice para copiar los bins      */

    CHECK_OPEN();

    /* Validar el puntero de entrada. */
    if (!bins) {
        errno = EINVAL;
        return -1;
    }

    /* Inicializar toda la struct a 0 para limpiar posible basura en el stack.
     * Importante: la struct tiene 36×4 + 4 = 148 bytes; memset garantiza que
     * los bins no activos (si n_bins < LCD_MAX_BINS) estén en cero.          */
    memset(&cmd, 0, sizeof(cmd));

    /* Truncar n_bins al máximo soportado por el driver y el display.
     * El driver también hace esta validación, pero hacerlo aquí evita
     * que se copien bytes basura a la struct en el paso siguiente.            */
    cmd.n_bins = (n_bins > LCD_MAX_BINS) ? LCD_MAX_BINS : n_bins;

    /* Copiar los conteos del array del llamador a la struct ioctl.
     * El driver leerá estos valores con copy_from_user en el kernel.         */
    for (i = 0; i < cmd.n_bins; i++)
        cmd.bins[i] = bins[i];

    if (ioctl(fd_lcd, IOCTL_LCD_SHOW_HISTOGRAM, &cmd) < 0) {
        fprintf(stderr, "[lcd_lib] lcd_show_histogram(n=%u) falló: %s\n",
                cmd.n_bins, strerror(errno));
        return -1;
    }

    return 0;
}
