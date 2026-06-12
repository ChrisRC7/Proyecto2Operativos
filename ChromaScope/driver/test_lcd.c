/*
 * test_lcd.c — Programa de prueba en espacio de usuario para el driver ST7735.
 *
 * Ejecuta una secuencia de pruebas visuales sobre la pantalla LCD TFT 160×128:
 *   1. Inicializar el display (secuencias de configuración del ST7735).
 *   2. Colores primarios: rojo, verde, azul, blanco, negro.
 *   3. Cuatro rectángulos de colores en los cuadrantes de la pantalla.
 *   4. Espectro de colores (rueda HSV completa como franja horizontal).
 *   5. Histograma HSV simulado con distribución tipo campana (bin dominante: verde).
 *   6. Apagar la pantalla (llenar de negro).
 *
 * Requiere que el driver chromascope_lcd.ko esté cargado:
 *   sudo insmod chromascope_lcd.ko
 *
 * Ejecutar:
 *   sudo ./test_lcd
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include <stdio.h>      /* printf, fprintf, perror: salida a consola y errores   */
#include <stdlib.h>     /* EXIT_FAILURE, EXIT_SUCCESS: códigos de retorno         */
#include <stdint.h>     /* uint8_t, uint16_t, uint32_t: tipos de ancho fijo       */
#include <fcntl.h>      /* O_RDWR: flag para abrir el dispositivo de lectura+escritura */
#include <unistd.h>     /* close(), sleep(): cerrar fd y esperar segundos         */
#include <sys/ioctl.h>  /* ioctl(): llamada al driver desde espacio de usuario    */

#include "chromascope_lcd.h"  /* constantes LCD_*, structs lcd_cmd_*, IOCTL_LCD_* */

/* ── Variables y helpers globales ───────────────────────────────────────── */

/* File descriptor global del dispositivo LCD — usado por los helpers.         */
static int lcd_fd = -1;

/*
 * check() — verifica el resultado de un ioctl y termina el programa si falla.
 *
 * Parámetros:
 *   label — nombre descriptivo de la operación (para el mensaje de error)
 *   ret   — valor retornado por ioctl() (< 0 indica error)
 *
 * Si ret < 0: perror imprime el error del sistema (strerror(errno)),
 *             se cierra el fd si estaba abierto, y se sale con EXIT_FAILURE.
 */
static void check(const char *label, int ret)
{
    if (ret < 0) {
        perror(label);          /* imprime: "<label>: <mensaje de error del SO>" */
        if (lcd_fd >= 0)
            close(lcd_fd);      /* cerrar el dispositivo antes de salir          */
        exit(EXIT_FAILURE);
    }
}

/*
 * fill() — rellena toda la pantalla con un color sólido usando IOCTL_LCD_FILL_SCREEN.
 *
 * Construye la struct lcd_cmd_fill con el color dado y la envía al driver.
 * El driver chromascope_lcd.c ejecuta lcd_fill_screen() que hace un fill_rect
 * de todo el display de una sola vez (CS baja una vez para todos los píxeles).
 */
static void fill(uint16_t color)
{
    /* Inicializar la struct con el color RGB565 solicitado.                    */
    struct lcd_cmd_fill f = { .color = color };

    /* ioctl enviará la struct al kernel; el driver la lee con copy_from_user(). */
    check("ioctl FILL_SCREEN", ioctl(lcd_fd, IOCTL_LCD_FILL_SCREEN, &f));
}

/*
 * rect() — dibuja un rectángulo relleno con IOCTL_LCD_DRAW_RECT.
 *
 * Parámetros:
 *   x, y  — esquina superior izquierda (en píxeles, origen en 0,0 = arriba-izquierda)
 *   w, h  — ancho y alto del rectángulo en píxeles
 *   color — color RGB565 del relleno
 */
static void rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    /* Rellenar todos los campos de la struct con los valores dados.            */
    struct lcd_cmd_rect r = { .x = x, .y = y, .w = w, .h = h, .color = color };

    /* Enviar al driver; internamente llama a lcd_fill_rect().                  */
    check("ioctl DRAW_RECT", ioctl(lcd_fd, IOCTL_LCD_DRAW_RECT, &r));
}

/* ── Programa principal ─────────────────────────────────────────────────── */

int main(void)
{
    int i;   /* variable de bucle para el espectro y el histograma             */

    /* Abrir el nodo de dispositivo del driver LCD.
     * O_RDWR: necesario para enviar ioctl (aunque no hay lectura/escritura real).
     * Falla si chromascope_lcd.ko no está cargado (el nodo /dev/chromascope no existe). */
    lcd_fd = open(LCD_DEV, O_RDWR);
    if (lcd_fd < 0) {
        perror("open " LCD_DEV);
        fprintf(stderr, "¿Está cargado el módulo?  →  sudo insmod chromascope_lcd.ko\n");
        return EXIT_FAILURE;
    }

    /* ── Prueba 1: Inicializar el display ST7735 ── */
    printf("1. Inicializando ST7735...\n");

    /* IOCTL_LCD_INIT envía las tres tablas de inicialización (rcmd1/2/3) al controlador:
     * - Reset de software (SWRESET) + salida de sleep (SLPOUT)
     * - Configuración de frame rate, control de potencia, orientación (MADCTL)
     * - Curvas de gamma y encendido del panel (DISPON)                         */
    check("ioctl INIT", ioctl(lcd_fd, IOCTL_LCD_INIT));
    printf("   OK — display 160x128 encendido (paisaje)\n\n");

    /* ── Prueba 2: Colores primarios — pantalla completa ── */
    printf("2. Prueba de colores primarios...\n");

    /* RGB565: cada color está definido en chromascope_lcd.h como constante __u16. */
    fill(LCD_RED);    printf("   Rojo\n");    sleep(1);  /* 0x001F en RGB565    */
    fill(LCD_GREEN);  printf("   Verde\n");   sleep(1);  /* 0x07E0             */
    fill(LCD_BLUE);   printf("   Azul\n");    sleep(1);  /* 0xF800             */
    fill(LCD_WHITE);  printf("   Blanco\n");  sleep(1);  /* 0xFFFF             */
    fill(LCD_BLACK);  printf("   Negro\n\n");             /* 0x0000             */

    /* ── Prueba 3: Cuatro rectángulos de colores (cuadrantes) ── */
    printf("3. Cuatro rectángulos de colores...\n");

    /* División de la pantalla 160×128 en 4 cuadrantes aproximados (74×58 px c/u).
     * Se dejan 4 px de margen en cada borde y 8 px de separación entre cuadrantes. */
    rect(  4,  4, 74, 58, LCD_RED);      /* cuadrante superior-izquierdo: rojo  */
    rect( 82,  4, 74, 58, LCD_GREEN);    /* cuadrante superior-derecho: verde   */
    rect(  4, 66, 74, 58, LCD_BLUE);     /* cuadrante inferior-izquierdo: azul  */
    rect( 82, 66, 74, 58, LCD_YELLOW);   /* cuadrante inferior-derecho: amarillo*/
    sleep(2);

    /* ── Prueba 4: Espectro de colores (rueda HSV como franja horizontal) ── */
    printf("4. Espectro de colores (rueda HSV)...\n");
    fill(LCD_BLACK);   /* limpiar pantalla antes del espectro                   */

    /* Recorrer cada columna de la pantalla (0 a LCD_WIDTH-1 = 0 a 159).       */
    for (i = 0; i < LCD_WIDTH; i++) {

        /* Mapear la columna i a un ángulo de matiz en [0°, 360°).              */
        uint32_t hue  = (uint32_t)i * 360u / (uint32_t)LCD_WIDTH;

        uint16_t color;

        /* Convertir el ángulo HSV (S=1, V=1) a RGB565 usando aritmética entera.
         * Este es el mismo algoritmo que hue_to_rgb565() en chromascope_lcd.c. */
        uint32_t sector = hue / 60u;          /* sector de 60° (0–5)            */
        uint32_t frac   = hue % 60u;          /* fracción dentro del sector     */
        uint32_t x_comp = frac * 255u / 60u;  /* interpolación lineal [0, 255]  */
        uint32_t r = 0, g = 0, b = 0;

        /* Tabla de sectores: qué componentes varían en cada segmento de 60°.  */
        switch (sector) {
        case 0: r = 255; g = x_comp; b = 0;           break; /* rojo→amarillo  */
        case 1: r = 255 - x_comp; g = 255; b = 0;     break; /* amarillo→verde */
        case 2: r = 0; g = 255; b = x_comp;            break; /* verde→cian     */
        case 3: r = 0; g = 255 - x_comp; b = 255;     break; /* cian→azul      */
        case 4: r = x_comp; g = 0; b = 255;            break; /* azul→magenta   */
        default: r = 255; g = 0; b = 255 - x_comp;    break; /* magenta→rojo   */
        }

        /* Empaquetar R, G, B en formato RGB565 (16 bits):
         * Bits 15–11: 5 bits de Rojo   → (r & 0xF8) << 8
         * Bits 10–5:  6 bits de Verde  → (g & 0xFC) << 3
         * Bits  4–0:  5 bits de Azul   → b >> 3                               */
        color = (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));

        /* Dibujar una franja vertical de 1×40 px en la columna i.
         * La franja empieza en y=44 (centrada verticalmente en 128 px).       */
        rect((uint16_t)i, 44, 1, 40, color);
    }
    sleep(2);

    /* ── Prueba 5: Histograma HSV simulado (36 bins) ── */
    printf("5. Histograma HSV simulado (36 bins)...\n");
    {
        struct lcd_cmd_histogram hist;
        hist.n_bins = 36;   /* 36 bins × 10°/bin = 360° de la rueda de color   */

        /*
         * Generar una distribución tipo campana centrada en bin 12 (≈ 120° = verde).
         * Fórmula: conteo = max(0, 100 - |i - 12| × 9)
         *   - bin 12:  100 (máximo)
         *   - bin 11/13: 91
         *   - bin 10/14: 82
         *   - ...
         *   - bin  3/21: 1
         *   - bins fuera de rango |i-12| ≥ 10: 0
         * Esto simula una imagen donde el verde es el color dominante.
         */
        for (i = 0; i < 36; i++) {
            int diff = i - 12;               /* distancia del bin al centro     */
            if (diff < 0) diff = -diff;      /* valor absoluto                  */

            /* Asignar el conteo: campana que decae linealmente hacia afuera.   */
            hist.bins[i] = (diff < 10) ? (uint32_t)(100 - diff * 9) : 0u;
        }

        /* IOCTL_LCD_SHOW_HISTOGRAM: el driver dibuja barras verticales.
         * Cada barra tiene el color HSV del bin que representa.
         * La altura de la barra es proporcional al conteo del bin.             */
        check("ioctl SHOW_HISTOGRAM", ioctl(lcd_fd, IOCTL_LCD_SHOW_HISTOGRAM, &hist));
    }
    sleep(3);

    /* ── Prueba 6: Apagar la pantalla ── */
    printf("6. Pantalla apagada (negro).\n");
    fill(LCD_BLACK);   /* limpiar la pantalla con negro al finalizar             */

    /* Cerrar el file descriptor del dispositivo LCD.                           */
    close(lcd_fd);
    printf("\nTest completado.\n");
    return EXIT_SUCCESS;
}
