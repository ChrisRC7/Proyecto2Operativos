// SPDX-License-Identifier: GPL-2.0
/*
 * chromascope_lcd.c — Driver de kernel para pantalla TFT ST7735 (128×160 px)
 *
 * Hardware objetivo : Raspberry Pi 4 · BCM2711
 * Comunicación      : SPI por software (bit-banging) via registros GPIO directos
 *                     — sin usar gpiolib ni el subsistema SPI del kernel —
 *
 * Pines por defecto (modificables con parámetros del módulo):
 *   CLK  → GPIO 11  (pin físico 23)
 *   MOSI → GPIO 10  (pin físico 19)
 *   CS   → GPIO  8  (pin físico 24)
 *   DC   → GPIO 25  (pin físico 22)
 *   RST  → GPIO 27  (pin físico 13)
 *
 * Secuencias de inicialización basadas en la librería Adafruit ST7735
 * (traducidas de C++ a C puro, sin PROGMEM ni herencia de clases).
 *
 * Dispositivo: /dev/chromascope
 * API:         chromascope_lcd.h
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

/* Macros de módulo: module_init/exit, MODULE_LICENSE, module_param, etc. */
#include <linux/module.h>
/* pr_info, pr_err y tipos base del kernel (u8, u16, u32, etc.)           */
#include <linux/kernel.h>
/* LINUX_VERSION_CODE y KERNEL_VERSION(): detectar versión en tiempo de   */
/* compilación — usado para el cambio de API de class_create en ≥6.4      */
#include <linux/version.h>
/* Atributos __init y __exit: marcan funciones para secciones especiales   */
/* de la imagen del kernel; liberadas de RAM tras la inicialización        */
#include <linux/init.h>
/* struct file_operations, alloc/unregister_chrdev_region                 */
#include <linux/fs.h>
/* struct cdev, cdev_init, cdev_add, cdev_del — dispositivo de caracteres */
#include <linux/cdev.h>
/* struct class, device_create, device_destroy, class_create/destroy      */
/* → udev usa esta información para crear /dev/chromascope automáticamente */
#include <linux/device.h>
/* copy_from_user / copy_to_user: copia segura entre espacio de usuario    */
/* y espacio del kernel; retorna bytes NO copiados (0 = éxito)             */
#include <linux/uaccess.h>
/* ioremap / iounmap: mapea dirección física a virtual del kernel          */
/* ioread32 / iowrite32: lectura/escritura atómica de 32 bits sobre MMIO  */
#include <linux/io.h>
/* msleep(ms): retardo que cede el procesador — seguro en contexto de      */
/* proceso (softirq/interrupt NO pueden usarlo; usar udelay allí)          */
#include <linux/delay.h>
/* DEFINE_MUTEX, mutex_lock_interruptible, mutex_unlock: exclusión mutua   */
/* que protege el bus SPI contra accesos simultáneos desde varios procesos */
#include <linux/mutex.h>
/* IS_ERR(ptr) / PTR_ERR(ptr): codifica/decodifica errores en punteros.   */
/* El kernel devuelve punteros como -ENOMEM cuando falla class_create, etc.*/
#include <linux/err.h>

/* API pública del driver: structs ioctl, constantes de color, dimensiones */
#include "chromascope_lcd.h"

/* ── Nombre del dispositivo ──────────────────────────────────────────────── */
#define DEVICE_NAME  "chromascope"   /* nombre del nodo /dev/chromascope          */
#define CLASS_NAME   "chromascope"   /* nombre de la clase en /sys/class/         */

/* ── Registros GPIO del BCM2711 (base física 0xFE200000) ─────────────────── */

/* Dirección física base del bloque GPIO en la memoria del BCM2711           */
/* (RPi 4: rango 0xFE200000–0xFE2000B3, sólo necesitamos 0x100 bytes)        */
#define GPIO_PHYS_BASE  0xFE200000U

/* Tamaño del bloque a mapear: cubre hasta GPLEV0 (offset 0x34) + margen    */
#define GPIO_MAP_SIZE   0x100U

/* GPFSEL0 (Function Select 0): configura la función de los GPIO 0–9.       */
/* Cada pin usa 3 bits dentro del registro de 32 bits:                       */
/*   000 = INPUT, 001 = OUTPUT, 010-111 = funciones alternativas (SPI,I2C…) */
/* Los registros GPFSEL1/2 están en GPFSEL0+4 y GPFSEL0+8 (pines 10–19/20–29)*/
#define GPFSEL0         0x00U

/* GPSET0: escribir un 1 en el bit N pone GPIO-N en HIGH (3.3 V).            */
/* Escribir 0 no tiene efecto — así se puede activar un pin sin leer antes   */
#define GPSET0          0x1CU

/* GPCLR0: escribir un 1 en el bit N pone GPIO-N en LOW (0 V).               */
/* Misma lógica de "solo-escribir" que GPSET0                                */
#define GPCLR0          0x28U

/* GPLEV0: leer el bit N devuelve el nivel actual de GPIO-N (0 ó 1)          */
#define GPLEV0          0x34U

/* Valor de los 3 bits de GPFSEL para configurar un pin como SALIDA           */
#define GPIO_FUNC_OUT   0x1U

/* ── Pines SPI + control (parámetros del módulo) ─────────────────────────── */

/* Números BCM de los pines usados. Se pueden cambiar al cargar el módulo:   */
/*   sudo insmod chromascope_lcd.ko pin_dc=24 pin_clk=23                     */
static int pin_clk  = 11;  /* Señal de reloj SPI (CLK/SCK): GPIO 11, pin físico 23 */
static int pin_mosi = 10;  /* Datos del maestro al esclavo (DIN): GPIO 10, pin 19  */
static int pin_cs   =  8;  /* Chip Select — activo en LOW: GPIO 8, pin físico 24  */
static int pin_dc   = 25;  /* Data/Command: LOW=comando, HIGH=dato: GPIO 25, pin 22*/
static int pin_rst  = 27;  /* Reset hardware del ST7735 (activo en LOW): GPIO 27  */

/* Registrar cada variable como parámetro legible (0444 = solo lectura en sysfs) */
module_param(pin_clk,  int, 0444);
module_param(pin_mosi, int, 0444);
module_param(pin_cs,   int, 0444);
module_param(pin_dc,   int, 0444);
module_param(pin_rst,  int, 0444);
/* Descripción que aparece en modinfo y en /sys/module/chromascope_lcd/parameters/ */
MODULE_PARM_DESC(pin_clk,  "GPIO CLK  (defecto: 11 — pin físico 23)");
MODULE_PARM_DESC(pin_mosi, "GPIO MOSI (defecto: 10 — pin físico 19)");
MODULE_PARM_DESC(pin_cs,   "GPIO CS   (defecto:  8 — pin físico 24)");
MODULE_PARM_DESC(pin_dc,   "GPIO DC   (defecto: 25 — pin físico 22)");
MODULE_PARM_DESC(pin_rst,  "GPIO RST  (defecto: 27 — pin físico 13)");

/* ── Variables del módulo ────────────────────────────────────────────────── */

/* Dirección virtual que mapea a 0xFE200000: base de los registros GPIO      */
static void __iomem   *gpio_base;

/* Número mayor+menor asignado dinámicamente por alloc_chrdev_region()       */
static dev_t           dev_num;

/* Estructura cdev: representa el dispositivo de caracteres en el kernel     */
static struct cdev     lcd_cdev;

/* Clase en /sys/class/chromascope/: udev la usa para crear /dev/chromascope */
static struct class   *lcd_class;

/* Nodo /dev/chromascope generado por device_create()                        */
static struct device  *lcd_device;

/* Mutex que serializa el acceso al bus SPI (bit-bang no es re-entrante)     */
static DEFINE_MUTEX(lcd_mutex);

/* ── Comandos ST7735 / ST77XX ─────────────────────────────────────────────── */
#define ST77XX_SWRESET  0x01u
#define ST77XX_SLPOUT   0x11u
#define ST77XX_NORON    0x13u
#define ST77XX_INVOFF   0x20u
#define ST77XX_INVON    0x21u
#define ST77XX_DISPON   0x29u
#define ST77XX_CASET    0x2Au   /* Column Address Set */
#define ST77XX_RASET    0x2Bu   /* Row Address Set    */
#define ST77XX_RAMWR    0x2Cu   /* Memory Write       */
#define ST77XX_MADCTL   0x36u   /* Memory Access Control (orientación) */
#define ST77XX_COLMOD   0x3Au   /* Pixel Format Set   */

#define ST7735_FRMCTR1  0xB1u
#define ST7735_FRMCTR2  0xB2u
#define ST7735_FRMCTR3  0xB3u
#define ST7735_INVCTR   0xB4u
#define ST7735_PWCTR1   0xC0u
#define ST7735_PWCTR2   0xC1u
#define ST7735_PWCTR3   0xC2u
#define ST7735_PWCTR4   0xC3u
#define ST7735_PWCTR5   0xC4u
#define ST7735_VMCTR1   0xC5u
#define ST7735_GMCTRP1  0xE0u   /* Gamma Adjustment + (polaridad positiva) */
#define ST7735_GMCTRN1  0xE1u   /* Gamma Adjustment - (polaridad negativa) */

/* Bits del registro MADCTL (Memory Data Access Control, cmd 0x36)          */
/*   Controlan cómo el controlador recorre su GRAM al escribir píxeles       */
#define MADCTL_MY   0x80u   /* Bit 7: Mirror Y — invierte orden de filas       */
#define MADCTL_MX   0x40u   /* Bit 6: Mirror X — invierte orden de columnas    */
#define MADCTL_MV   0x20u   /* Bit 5: Row/Col Exchange — transpone X⇔Y (rota 90°) */
#define MADCTL_BGR  0x08u   /* Bit 3: orden BGR en vez de RGB (físico del panel) */

/*
 * ST_CMD_DELAY (0x80): bit de bandera en el campo n_args de la tabla de init.
 * Indica que después de los datos del comando viene un byte de retardo en ms.
 * Si el byte de retardo es 255 → se usa 500 ms.
 */
#define ST_CMD_DELAY  0x80u

/* ── Primitivas GPIO (acceso directo a registros BCM2711) ────────────────── */

static inline void gpio_set_func(int pin, unsigned int func)
{
    unsigned int reg   = (unsigned int)pin / 10u;
    unsigned int shift = ((unsigned int)pin % 10u) * 3u;
    u32 val;

    val  = ioread32(gpio_base + GPFSEL0 + reg * 4u);
    val &= ~(0x7u << shift);
    val |=  (func & 0x7u) << shift;
    iowrite32(val, gpio_base + GPFSEL0 + reg * 4u);
}

static inline void gpio_high(int pin)
{
    iowrite32(1u << (unsigned int)pin, gpio_base + GPSET0);
}

static inline void gpio_low(int pin)
{
    iowrite32(1u << (unsigned int)pin, gpio_base + GPCLR0);
}

/* ── SPI bit-bang: Modo 0 (CPOL=0, CPHA=0), MSB primero ─────────────────── */
/*
 * CPOL=0: el reloj está en LOW cuando no hay transferencia.
 * CPHA=0: el dato se muestrea en el flanco ASCENDENTE de CLK.
 * MSB first: se envía el bit más significativo (bit 7) primero.
 *
 * El ST7735 acepta hasta 66 MHz en escritura. La latencia de cada
 * iowrite32 es ~100 ns en la RPi 4, dando ~3–5 MHz efectivos — suficiente.
 * No se añaden retardos explícitos entre bits.
 */
static void spi_write_byte(u8 byte)
{
    int i;

    /* Iterar desde bit 7 (MSB) hasta bit 0 (LSB)                              */
    for (i = 7; i >= 0; i--) {

        /* Poner MOSI al nivel del bit actual antes de subir el reloj           */
        if (byte & (1u << i))
            gpio_high(pin_mosi);  /* bit = 1: MOSI en HIGH                     */
        else
            gpio_low(pin_mosi);   /* bit = 0: MOSI en LOW                      */

        /* Flanco ascendente: el ST7735 lee MOSI en este momento (CPHA=0)       */
        gpio_high(pin_clk);

        /* Flanco descendente: prepara el reloj para el siguiente bit           */
        gpio_low(pin_clk);
    }
}

/* ── Comunicación con el ST7735 ──────────────────────────────────────────── */

/*
 * lcd_write_cmd() — envía un byte de COMANDO al ST7735.
 * Protocolo: DC=LOW indica al controlador que el byte es una instrucción,
 * no un dato de píxel. CS se baja antes y se sube después de cada byte.
 */
static void lcd_write_cmd(u8 cmd)
{
    gpio_low(pin_dc);         /* DC=0: modo COMANDO (no dato)                  */
    gpio_low(pin_cs);         /* CS=0: seleccionar chip (iniciar transacción)  */
    spi_write_byte(cmd);      /* Transmitir el byte de comando por SPI         */
    gpio_high(pin_cs);        /* CS=1: deseleccionar (fin de transacción)      */
}

/*
 * lcd_write_data() — envía un byte de DATO al ST7735.
 * DC=HIGH indica que el byte es argumento del último comando enviado
 * o un píxel si estamos dentro de un ciclo RAMWR.
 */
static void lcd_write_data(u8 data)
{
    gpio_high(pin_dc);        /* DC=1: modo DATO                               */
    gpio_low(pin_cs);         /* CS=0: seleccionar chip                        */
    spi_write_byte(data);     /* Transmitir el byte de dato                    */
    gpio_high(pin_cs);        /* CS=1: deseleccionar                           */
}

/*
 * lcd_write_data16() — envía un color RGB565 (16 bits) como dos bytes.
 * El byte alto (bits 15–8) va primero: contiene los 5 bits de Rojo y
 * los 3 bits altos de Verde. El byte bajo contiene 3 bits bajos de Verde
 * y los 5 bits de Azul. CS se baja una sola vez para los dos bytes.
 */
static void lcd_write_data16(u16 data)
{
    gpio_high(pin_dc);                  /* DC=1: modo DATO                     */
    gpio_low(pin_cs);                   /* CS=0: inicio de transacción         */
    spi_write_byte((u8)(data >> 8));    /* Byte alto: RRRRR GGG                */
    spi_write_byte((u8)(data & 0xFFu)); /* Byte bajo: GGG BBBBB                */
    gpio_high(pin_cs);                  /* CS=1: fin de transacción            */
}

/* ── Ejecutar tabla de comandos de inicialización ────────────────────────── */
/*
 * Formato de la tabla (igual al de Adafruit, sin PROGMEM):
 *
 *   [N_cmds]
 *   repetir N_cmds veces:
 *     [cmd_byte]
 *     [n_args | ST_CMD_DELAY?]    ← bit 7 indica que hay retardo al final
 *     [arg0] [arg1] ... [argN]
 *     [delay_ms]                  ← solo si el bit ST_CMD_DELAY estaba activo
 *                                    (255 = 500 ms)
 */
static void lcd_exec_cmd_list(const u8 *list)
{
    u8       n_cmds, cmd, n_args, delay_flag;
    u16      delay_ms;
    int      i;

    /* Primer byte de la tabla: número total de comandos                      */
    n_cmds = *list++;

    while (n_cmds--) {    /* procesar cada entrada de la tabla                 */

        cmd        = *list++;      /* byte del comando ST7735/ST77XX            */
        n_args     = *list++;      /* cantidad de args + posible flag de retardo*/

        /* Separar el bit de bandera ST_CMD_DELAY (bit 7) del conteo real       */
        delay_flag = n_args & (u8)ST_CMD_DELAY;

        /* Quitar el bit de bandera para obtener el número real de argumentos   */
        n_args    &= ~(u8)ST_CMD_DELAY;

        /* Enviar el opcode del comando (DC=0)                                   */
        lcd_write_cmd(cmd);

        /* Enviar cada byte de argumento como dato (DC=1)                        */
        for (i = 0; i < n_args; i++)
            lcd_write_data(*list++);   /* incrementar puntero en cada arg       */

        /* Si el flag estaba activo, leer el byte de espera y dormir            */
        if (delay_flag) {
            delay_ms = *list++;         /* byte de milisegundos a esperar       */
            if (delay_ms == 255u)       /* valor especial: 255 → usar 500 ms    */
                delay_ms = 500u;
            msleep(delay_ms);           /* ceder la CPU durante el retardo      */
        }
    }
}

/* ── Secuencias de inicialización ST7735R — tab rojo, 128×160 ─────────────── */
/*
 * Traducción directa de los arrays Rcmd1, Rcmd2red y Rcmd3 de la librería
 * Adafruit ST7735 (github.com/adafruit/Adafruit-ST7735-Library).
 */

/* Parte 1: reset de software, frame rate, inversión, potencia, MADCTL, color */
static const u8 st7735_rcmd1[] = {
    15,                                             /* 15 comandos */
    ST77XX_SWRESET,  ST_CMD_DELAY,                  /*  1: Software reset */
        150,
    ST77XX_SLPOUT,   ST_CMD_DELAY,                  /*  2: Salir de sleep */
        255,                                        /*     500 ms */
    ST7735_FRMCTR1,  3,                             /*  3: Frame rate modo normal */
        0x01, 0x2C, 0x2D,
    ST7735_FRMCTR2,  3,                             /*  4: Frame rate modo idle */
        0x01, 0x2C, 0x2D,
    ST7735_FRMCTR3,  6,                             /*  5: Frame rate modo parcial */
        0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,
    ST7735_INVCTR,   1,                             /*  6: Control de inversión */
        0x07,
    ST7735_PWCTR1,   3,                             /*  7: Control de potencia */
        0xA2, 0x02, 0x84,
    ST7735_PWCTR2,   1,                             /*  8: Control de potencia */
        0xC5,
    ST7735_PWCTR3,   2,                             /*  9: Control de potencia */
        0x0A, 0x00,
    ST7735_PWCTR4,   2,                             /* 10: Control de potencia */
        0x8A, 0x2A,
    ST7735_PWCTR5,   2,                             /* 11: Control de potencia */
        0x8A, 0xEE,
    ST7735_VMCTR1,   1,                             /* 12: VCOM control */
        0x0E,
    ST77XX_INVOFF,   0,                             /* 13: Sin inversión de imagen */
    ST77XX_MADCTL,   1,                             /* 14: Orientación de memoria */
        (MADCTL_MY | MADCTL_MV | MADCTL_BGR),       /*     Paisaje, colores BGR  */
    ST77XX_COLMOD,   1,                             /* 15: Formato de píxel 16-bit */
        0x05
};

/* Parte 2: ventana de dirección para 128×160 (tab rojo, colstart=0 rowstart=0) */
static const u8 st7735_rcmd2_red[] = {
    2,
    ST77XX_CASET,  4,              /*  1: Columnas 0–159 (paisaje) */
        0x00, 0x00, 0x00, 0x9F,
    ST77XX_RASET,  4,              /*  2: Filas    0–127 (paisaje) */
        0x00, 0x00, 0x00, 0x7F
};

/* Parte 3: gamma y encendido del display */
static const u8 st7735_rcmd3[] = {
    4,
    ST7735_GMCTRP1, 16,            /*  1: Gamma positivo */
        0x02, 0x1C, 0x07, 0x12,
        0x37, 0x32, 0x29, 0x2D,
        0x29, 0x25, 0x2B, 0x39,
        0x00, 0x01, 0x03, 0x10,
    ST7735_GMCTRN1, 16,            /*  2: Gamma negativo */
        0x03, 0x1D, 0x07, 0x06,
        0x2E, 0x2C, 0x29, 0x2D,
        0x2E, 0x2E, 0x37, 0x3F,
        0x00, 0x00, 0x02, 0x10,
    ST77XX_NORON,   ST_CMD_DELAY,  /*  3: Display normal on */
        10,
    ST77XX_DISPON,  ST_CMD_DELAY,  /*  4: Display encendido */
        100
};

/* ── Inicialización completa del hardware ────────────────────────────────── */
static void lcd_hw_init(void)
{
    /*
     * Hard reset por hardware: se pulsa RST a LOW durante 10 ms.
     * Esto reinicia el controlador ST7735 a su estado de fábrica,
     * borrando cualquier configuración previa que pueda haber quedado.
     * La secuencia: HIGH (estado reposo) → LOW 10 ms → HIGH → esperar 200 ms.
     * Los 200 ms finales permiten que el oscilador interno del ST7735 estabilice.
     */
    gpio_high(pin_rst);   /* RST en reposo (HIGH)                              */
    msleep(10);           /* breve pausa de estabilización inicial             */
    gpio_low(pin_rst);    /* pulso LOW: activa el reset del controlador        */
    msleep(10);           /* mantener el reset activo mínimo 10 ms            */
    gpio_high(pin_rst);   /* liberar el reset                                  */
    msleep(200);          /* esperar que el ST7735 complete su arranque interno*/

    /* Enviar la secuencia completa de inicialización en tres partes:          */
    lcd_exec_cmd_list(st7735_rcmd1);      /* Frame rate, potencia, MADCTL, color*/
    lcd_exec_cmd_list(st7735_rcmd2_red);  /* Ventana de dirección (CASET/RASET) */
    lcd_exec_cmd_list(st7735_rcmd3);      /* Curvas gamma + encendido del panel  */

    pr_info("chromascope_lcd: display ST7735 inicializado (160×128 px, paisaje)\n");
}

/* ── Primitivas de dibujo ────────────────────────────────────────────────── */

/*
 * lcd_set_window() — define la ventana activa de escritura de píxeles.
 *
 * El ST7735 usa dos comandos para delimitar el rectángulo destino:
 *   CASET (Column Address Set): columna inicial y final (eje X).
 *   RASET (Row Address Set):    fila inicial y final    (eje Y).
 * Tras enviar ambos, RAMWR (Memory Write) pone el controlador en modo
 * de recepción de datos de píxel: los bytes siguientes se escriben
 * en el GRAM de izquierda a derecha y de arriba a abajo dentro de la ventana.
 *
 * Cada coordenada se envía como dos bytes (byte alto = 0x00, byte bajo = coord)
 * porque el ST7735 trabaja con direcciones de 16 bits.
 */
static void lcd_set_window(u16 x0, u16 y0, u16 x1, u16 y1)
{
    /* CASET: columna inicial (x0) y columna final (x1)                         */
    lcd_write_cmd(ST77XX_CASET);
    lcd_write_data(0x00); lcd_write_data((u8)x0);  /* x0: byte alto + byte bajo */
    lcd_write_data(0x00); lcd_write_data((u8)x1);  /* x1: byte alto + byte bajo */

    /* RASET: fila inicial (y0) y fila final (y1)                               */
    lcd_write_cmd(ST77XX_RASET);
    lcd_write_data(0x00); lcd_write_data((u8)y0);  /* y0: byte alto + byte bajo */
    lcd_write_data(0x00); lcd_write_data((u8)y1);  /* y1: byte alto + byte bajo */

    /* RAMWR: iniciar escritura en GRAM; los datos de color siguen a continuación*/
    lcd_write_cmd(ST77XX_RAMWR);
}

/*
 * lcd_fill_rect() — dibuja un rectángulo sólido del color indicado.
 *
 * Optimización clave: CS se mantiene en LOW durante toda la transmisión
 * de píxeles (se baja una sola vez y se sube una sola vez). Esto evita
 * el overhead de gpio_low/gpio_high por cada uno de los w*h píxeles,
 * lo que puede ser miles de ciclos en rectángulos grandes.
 *
 * También se pre-calculan hi y lo del color una sola vez fuera del bucle.
 */
static void lcd_fill_rect(u16 x, u16 y, u16 w, u16 h, u16 color)
{
    u32 total, i;
    u8  hi = (u8)(color >> 8);       /* byte alto del color RGB565            */
    u8  lo = (u8)(color & 0xFFu);    /* byte bajo del color RGB565            */

    /* Descartar llamadas fuera de los límites de la pantalla                  */
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w == 0 || h == 0)
        return;

    /* Recortar el rectángulo si se sale de los bordes                          */
    if (x + w > LCD_WIDTH)  w = (u16)(LCD_WIDTH  - x);  /* recortar ancho      */
    if (y + h > LCD_HEIGHT) h = (u16)(LCD_HEIGHT - y);  /* recortar alto       */

    /* Definir la ventana de escritura en el controlador                        */
    lcd_set_window(x, y, (u16)(x + w - 1u), (u16)(y + h - 1u));

    /* Número total de píxeles a escribir (cada uno = 2 bytes)                 */
    total = (u32)w * (u32)h;

    /* Mantener DC=1 (datos) y CS=0 durante toda la transmisión               */
    gpio_high(pin_dc);    /* todos los bytes siguientes son datos de píxel      */
    gpio_low(pin_cs);     /* CS baja UNA sola vez para todos los píxeles        */

    for (i = 0; i < total; i++) {
        spi_write_byte(hi);  /* byte alto del color                             */
        spi_write_byte(lo);  /* byte bajo del color                             */
    }
    gpio_high(pin_cs);   /* CS=1: fin de la ráfaga de píxeles                  */
}

/*
 * lcd_draw_pixel() — dibuja un único píxel en (x, y).
 * Define una ventana de 1x1 y envía el color como dos bytes de dato.
 */
static void lcd_draw_pixel(u16 x, u16 y, u16 color)
{
    /* Ignorar coordenadas fuera de pantalla                                    */
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;

    /* Ventana de 1 píxel: x0==x1, y0==y1                                      */
    lcd_set_window(x, y, x, y);

    /* Enviar el color RGB565 en dos bytes                                      */
    lcd_write_data16(color);
}

/*
 * lcd_fill_screen() — rellena toda la pantalla con un color.
 * Usa lcd_fill_rect con la ventana completa (0,0) a (WIDTH-1, HEIGHT-1).
 */
static void lcd_fill_screen(u16 color)
{
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);  /* rectángulo completo  */
}

/* ── Conversión de matiz (0–359°) a color RGB565 ─────────────────────────── */
/*
 * S = 1, V = 1 (color saturado al máximo).
 * Implementado íntegramente con aritmética entera — sin flotantes.
 *
 * La rueda de colores HSV tiene 6 sectores de 60°:
 *   0°  → Rojo     60°  → Amarillo  120° → Verde
 *   180°→ Cian     240° → Azul      300° → Magenta
 *
 * Dentro de cada sector, una componente sube de 0→255 o baja de 255→0
 * mientras las otras son 0 o 255.
 */
static u16 hue_to_rgb565(u32 hue_deg)
{
    u32 sector, frac, r, g, b, x;

    /* Normalizar el matiz al rango [0, 359°]                                   */
    hue_deg %= 360u;

    /* Determinar en qué sector de 60° cae el matiz (0–5)                      */
    sector   = hue_deg / 60u;

    /* Fracción dentro del sector: cuántos grados llevamos desde su inicio      */
    frac     = hue_deg % 60u;      /* [0, 59°] */

    /* Componente que varía linealmente dentro del sector: escalar de 0 a 255  */
    x        = frac * 255u / 60u;  /* interpolación lineal entera [0, 255]      */

    /*
     * Tabla de sectores (S=1, V=1):
     *   Sector 0 (  0–59°): R sube   → rojo puro a amarillo
     *   Sector 1 ( 60–119°): R baja   → amarillo a verde
     *   Sector 2 (120–179°): B sube   → verde a cian
     *   Sector 3 (180–239°): G baja   → cian a azul
     *   Sector 4 (240–299°): R sube   → azul a magenta
     *   Sector 5 (300–359°): B baja   → magenta a rojo
     */
    switch (sector) {
    case 0:  r = 255;       g = x;         b = 0;         break; /* rojo→amarillo */
    case 1:  r = 255u - x;  g = 255;       b = 0;         break; /* amarillo→verde*/
    case 2:  r = 0;         g = 255;       b = x;         break; /* verde→cian    */
    case 3:  r = 0;         g = 255u - x;  b = 255;       break; /* cian→azul     */
    case 4:  r = x;         g = 0;         b = 255;       break; /* azul→magenta  */
    default: r = 255;       g = 0;         b = 255u - x;  break; /* magenta→rojo  */
    }

    /*
     * Empaquetar R, G, B en formato RGB565:
     *   Bits 15–11: 5 bits de Rojo   (tomar bits 7–3: >> 3 después de & 0xF8)
     *   Bits 10–5:  6 bits de Verde  (tomar bits 7–2: >> 2 después de & 0xFC)
     *   Bits  4–0:  5 bits de Azul   (tomar bits 7–3: >> 3)
     */
    return (u16)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/* ── Visualización del histograma HSV ────────────────────────────────────── */
/*
 * Cada bin i representa el ángulo de matiz: H = i × (360 / n_bins)
 * La barra se dibuja desde abajo hacia arriba con altura proporcional
 * al conteo del bin normalizado respecto al bin máximo.
 */
static void lcd_show_histogram(const struct lcd_cmd_histogram *hist)
{
    u32 max_count = 1u;   /* iniciar en 1 evita división por cero más adelante */
    u32 i, bar_h, x_pos, bar_w;
    u32 n = hist->n_bins; /* número de bins solicitado por el usuario           */

    /* Sanitizar n_bins: si es 0 o excede el máximo, usar LCD_MAX_BINS (36)    */
    if (n == 0 || n > LCD_MAX_BINS)
        n = LCD_MAX_BINS;

    /* Paso 1: limpiar la pantalla con fondo negro                              */
    lcd_fill_screen(LCD_BLACK);

    /* Paso 2: encontrar el bin con mayor conteo para usarlo como denominador   */
    /* de la normalización — así la barra más alta ocupa toda la altura        */
    for (i = 0; i < n; i++)
        if (hist->bins[i] > max_count)
            max_count = hist->bins[i];   /* guardar el máximo encontrado        */

    /* Paso 3: calcular el ancho de cada barra en píxeles.                     */
    /* Con 36 bins en 160 px: bar_w = 4 px por barra (más un hueco de 16 px)  */
    bar_w = (u32)LCD_WIDTH / n;
    if (bar_w == 0)
        bar_w = 1u;    /* garantizar al menos 1 píxel de ancho                  */

    /* Paso 4: dibujar cada barra de abajo hacia arriba                         */
    for (i = 0; i < n; i++) {

        /* Saltar bins vacíos (no dibujar nada)                                 */
        if (hist->bins[i] == 0)
            continue;

        /* Altura proporcional: escalar el conteo al rango [0, LCD_HEIGHT]      */
        bar_h = (hist->bins[i] * (u32)LCD_HEIGHT) / max_count;
        if (bar_h == 0)
            bar_h = 1u;     /* mínimo visible: nunca dibujar barra de altura 0  */

        /* Posición horizontal: distribuir barras equiespaciadas en el eje X     */
        x_pos = i * (u32)LCD_WIDTH / n;

        lcd_fill_rect(
            (u16)x_pos,                          /* columna de inicio           */
            (u16)((u32)LCD_HEIGHT - bar_h),       /* fila de inicio (desde abajo)*/
            (u16)bar_w,                           /* ancho de la barra           */
            (u16)bar_h,                           /* alto de la barra            */
            hue_to_rgb565(i * 360u / n)           /* color = matiz del bin i     */
        );
    }
}

/* ── file_operations ─────────────────────────────────────────────────────── */

static int lcd_fops_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int lcd_fops_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static long lcd_fops_ioctl(struct file *filp,
                            unsigned int cmd, unsigned long arg)
{
    struct lcd_cmd_fill      fill;
    struct lcd_cmd_pixel     pixel;
    struct lcd_cmd_rect      rect;
    struct lcd_cmd_histogram hist;
    int ret = 0;

    if (_IOC_TYPE(cmd) != LCD_MAGIC)
        return -ENOTTY;

    /* Proteger acceso concurrente al bus SPI */
    if (mutex_lock_interruptible(&lcd_mutex))
        return -ERESTARTSYS;

    switch (cmd) {

    case IOCTL_LCD_INIT:
        lcd_hw_init();
        break;

    case IOCTL_LCD_FILL_SCREEN:
        if (copy_from_user(&fill, (void __user *)arg, sizeof(fill))) {
            ret = -EFAULT;
            break;
        }
        lcd_fill_screen(fill.color);
        break;

    case IOCTL_LCD_DRAW_PIXEL:
        if (copy_from_user(&pixel, (void __user *)arg, sizeof(pixel))) {
            ret = -EFAULT;
            break;
        }
        lcd_draw_pixel(pixel.x, pixel.y, pixel.color);
        break;

    case IOCTL_LCD_DRAW_RECT:
        if (copy_from_user(&rect, (void __user *)arg, sizeof(rect))) {
            ret = -EFAULT;
            break;
        }
        lcd_fill_rect(rect.x, rect.y, rect.w, rect.h, rect.color);
        break;

    case IOCTL_LCD_SHOW_HISTOGRAM:
        if (copy_from_user(&hist, (void __user *)arg, sizeof(hist))) {
            ret = -EFAULT;
            break;
        }
        lcd_show_histogram(&hist);
        break;

    default:
        ret = -ENOTTY;
    }

    mutex_unlock(&lcd_mutex);
    return ret;
}

static const struct file_operations lcd_fops = {
    .owner          = THIS_MODULE,
    .open           = lcd_fops_open,
    .release        = lcd_fops_release,
    .unlocked_ioctl = lcd_fops_ioctl,
};

/* ── Configuración inicial de pines GPIO ─────────────────────────────────── */
static void lcd_setup_pins(void)
{
    /* Configurar todos los pines SPI+control como salidas digitales            */
    gpio_set_func(pin_clk,  GPIO_FUNC_OUT);  /* CLK: salida de reloj SPI       */
    gpio_set_func(pin_mosi, GPIO_FUNC_OUT);  /* MOSI/DIN: salida de datos SPI  */
    gpio_set_func(pin_cs,   GPIO_FUNC_OUT);  /* CS: selección de chip          */
    gpio_set_func(pin_dc,   GPIO_FUNC_OUT);  /* DC: selector dato/comando      */
    gpio_set_func(pin_rst,  GPIO_FUNC_OUT);  /* RST: reset del controlador     */

    /*
     * Estado de reposo del bus SPI (idle):
     * - CS=1: display deseleccionado (bus libre)
     * - DC=1: preparado para dato (se bajara cuando se necesite comando)
     * - RST=1: controlador no en reset (activo LOW)
     * - CLK=0: reloj en bajo (CPOL=0, estado de reposo del modo SPI 0)
     * - MOSI=0: línea de datos en bajo
     */
    gpio_high(pin_cs);    /* CS en reposo = HIGH (inactivo)                    */
    gpio_high(pin_dc);    /* DC en reposo = HIGH (dato)                        */
    gpio_high(pin_rst);   /* RST en reposo = HIGH (no-reset)                   */
    gpio_low(pin_clk);    /* CLK en reposo = LOW (CPOL=0)                      */
    gpio_low(pin_mosi);   /* MOSI en reposo = LOW                              */
}

/* ── module_init / module_exit ───────────────────────────────────────────── */

static int __init chromascope_lcd_init(void)
{
    int ret;

    /*
     * Paso 1: mapear los registros físicos GPIO del BCM2711 (0xFE200000)
     * al espacio de direcciones virtuales del kernel mediante ioremap.
     * Esto es obligatorio porque el kernel no permite acceder directamente
     * a direcciones físicas — todas las operaciones deben ir por la MMU.
     * En caso de fallo devuelve NULL y se aborta la carga.
     */
    gpio_base = ioremap(GPIO_PHYS_BASE, GPIO_MAP_SIZE);
    if (!gpio_base) {
        pr_err("chromascope_lcd: ioremap(0x%08X) falló\n", GPIO_PHYS_BASE);
        return -ENOMEM;  /* sin memoria virtual disponible                     */
    }

    /*
     * Paso 2: configurar los pines GPIO como salidas y llevarlos a
     * su estado de reposo (CS alto, CLK bajo, etc.).
     * Debe hacerse antes de la inicialización del hardware.
     */
    lcd_setup_pins();

    /*
     * Paso 3: reservar dinámicamente un número mayor (major) para el
     * dispositivo de caracteres. El kernel asigna el primer número libre.
     * Se reserva 1 número menor (minor=0) con nombre "chromascope".
     */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("chromascope_lcd: alloc_chrdev_region falló (%d)\n", ret);
        goto err_unmap;  /* deshacer ioremap antes de salir                    */
    }

    /*
     * Paso 4: inicializar y registrar el dispositivo de caracteres (cdev).
     * cdev_init asocia la tabla lcd_fops al objeto cdev.
     * cdev_add lo hace visible al VFS con el número asignado.
     */
    cdev_init(&lcd_cdev, &lcd_fops);   /* enlazar fops al cdev                */
    lcd_cdev.owner = THIS_MODULE;      /* evitar descarga mientras hay fds     */
    ret = cdev_add(&lcd_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("chromascope_lcd: cdev_add falló (%d)\n", ret);
        goto err_unreg;  /* deshacer chrdev_region                             */
    }

    /*
     * Paso 5: crear la clase del dispositivo en /sys/class/chromascope/
     * y el nodo /dev/chromascope mediante udev.
     *
     * API de class_create cambió en kernel 6.4: ya no acepta THIS_MODULE.
     * Se usa el preprocesador para soportar ambas versiones.
     */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    lcd_class = class_create(CLASS_NAME);           /* API ≥ 6.4              */
#else
    lcd_class = class_create(THIS_MODULE, CLASS_NAME); /* API < 6.4           */
#endif
    if (IS_ERR(lcd_class)) {
        ret = (int)PTR_ERR(lcd_class);  /* extraer el código de error          */
        pr_err("chromascope_lcd: class_create falló (%d)\n", ret);
        goto err_cdev;
    }

    /* device_create crea /dev/chromascope y notifica a udev para crear el nodo*/
    lcd_device = device_create(lcd_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(lcd_device)) {
        ret = (int)PTR_ERR(lcd_device);
        pr_err("chromascope_lcd: device_create falló (%d)\n", ret);
        goto err_class;
    }

    /* Imprimir mensajes de éxito visibles con dmesg                           */
    pr_info("chromascope_lcd: driver cargado — /dev/%s (major=%d)\n",
            DEVICE_NAME, MAJOR(dev_num));
    pr_info("chromascope_lcd: CLK=%d MOSI=%d CS=%d DC=%d RST=%d\n",
            pin_clk, pin_mosi, pin_cs, pin_dc, pin_rst);
    pr_info("chromascope_lcd: llama 'ioctl IOCTL_LCD_INIT' para encender la pantalla\n");
    return 0;   /* éxito: módulo cargado y /dev/chromascope disponible        */

    /*
     * Etiquetas de error: se deshacen los pasos en orden INVERSO al de
     * inicialización para dejar el sistema exactamente como estaba.
     * Este patrón "goto encadenado" es el idioma estándar de Linux.
     */
err_class:
    class_destroy(lcd_class);              /* destruir clase del sysfs         */
err_cdev:
    cdev_del(&lcd_cdev);                   /* quitar el cdev del VFS           */
err_unreg:
    unregister_chrdev_region(dev_num, 1);  /* liberar el número mayor          */
err_unmap:
    iounmap(gpio_base);                    /* liberar el mapeo de MMIO         */
    return ret;
}

/*
 * chromascope_lcd_exit() — limpieza al descargar con rmmod.
 * Los recursos se liberan en orden estrictamente inverso a como se crearon.
 */
static void __exit chromascope_lcd_exit(void)
{
    device_destroy(lcd_class, dev_num);   /* eliminar /dev/chromascope + udev  */
    class_destroy(lcd_class);             /* eliminar /sys/class/chromascope/  */
    cdev_del(&lcd_cdev);                  /* desregistrar el dispositivo cdev  */
    unregister_chrdev_region(dev_num, 1); /* liberar el número mayor           */
    iounmap(gpio_base);                   /* desmapear los registros GPIO      */
    pr_info("chromascope_lcd: driver descargado\n");
}

module_init(chromascope_lcd_init);
module_exit(chromascope_lcd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ChromaScope Team — CE 4303");
MODULE_DESCRIPTION("Driver ST7735 TFT LCD 128x160 para Raspberry Pi 4 (BCM2711) — SPI por software");
MODULE_VERSION("0.1");
