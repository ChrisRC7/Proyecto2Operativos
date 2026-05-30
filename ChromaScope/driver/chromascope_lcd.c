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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/err.h>

#include "chromascope_lcd.h"

/* ── Nombre del dispositivo ──────────────────────────────────────────────── */
#define DEVICE_NAME  "chromascope"
#define CLASS_NAME   "chromascope"

/* ── Registros GPIO del BCM2711 (base física 0xFE200000) ─────────────────── */
#define GPIO_PHYS_BASE  0xFE200000U
#define GPIO_MAP_SIZE   0x100U
#define GPFSEL0         0x00U   /* Function Select 0 (3 bits/pin, 10 pines/reg) */
#define GPSET0          0x1CU   /* Pin Output Set    — escribir 1 pone el pin HIGH */
#define GPCLR0          0x28U   /* Pin Output Clear  — escribir 1 pone el pin LOW  */
#define GPLEV0          0x34U   /* Pin Level         — leer nivel actual            */
#define GPIO_FUNC_OUT   0x1U

/* ── Pines SPI + control (parámetros del módulo) ─────────────────────────── */
static int pin_clk  = 11;
static int pin_mosi = 10;
static int pin_cs   =  8;
static int pin_dc   = 25;
static int pin_rst  = 27;

module_param(pin_clk,  int, 0444);
module_param(pin_mosi, int, 0444);
module_param(pin_cs,   int, 0444);
module_param(pin_dc,   int, 0444);
module_param(pin_rst,  int, 0444);
MODULE_PARM_DESC(pin_clk,  "GPIO CLK  (defecto: 11 — pin físico 23)");
MODULE_PARM_DESC(pin_mosi, "GPIO MOSI (defecto: 10 — pin físico 19)");
MODULE_PARM_DESC(pin_cs,   "GPIO CS   (defecto:  8 — pin físico 24)");
MODULE_PARM_DESC(pin_dc,   "GPIO DC   (defecto: 25 — pin físico 22)");
MODULE_PARM_DESC(pin_rst,  "GPIO RST  (defecto: 27 — pin físico 13)");

/* ── Variables del módulo ────────────────────────────────────────────────── */
static void __iomem   *gpio_base;
static dev_t           dev_num;
static struct cdev     lcd_cdev;
static struct class   *lcd_class;
static struct device  *lcd_device;
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

/* Bits del registro MADCTL */
#define MADCTL_MY   0x80u   /* Mirror Y (invertir filas)    */
#define MADCTL_MX   0x40u   /* Mirror X (invertir columnas) */
#define MADCTL_MV   0x20u   /* Row/Col exchange (transponer) */
#define MADCTL_BGR  0x08u   /* Orden de color BGR (en vez de RGB) */

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
 * El ST7735 muestrea MOSI en el flanco ascendente de CLK.
 * No se agregan retardos explícitos; la latencia de iowrite32 (~100 ns
 * cada llamada) da una frecuencia SPI efectiva de ~3–5 MHz, dentro del
 * límite de escritura del ST7735 (66 MHz) y del límite de lectura (15 MHz).
 */
static void spi_write_byte(u8 byte)
{
    int i;

    for (i = 7; i >= 0; i--) {
        if (byte & (1u << i))
            gpio_high(pin_mosi);
        else
            gpio_low(pin_mosi);

        gpio_high(pin_clk);   /* flanco ascendente — ST7735 muestrea aquí */
        gpio_low(pin_clk);    /* flanco descendente */
    }
}

/* ── Comunicación con el ST7735 ──────────────────────────────────────────── */

/* Envía un byte de COMANDO (DC=0) */
static void lcd_write_cmd(u8 cmd)
{
    gpio_low(pin_dc);
    gpio_low(pin_cs);
    spi_write_byte(cmd);
    gpio_high(pin_cs);
}

/* Envía un byte de DATO (DC=1) */
static void lcd_write_data(u8 data)
{
    gpio_high(pin_dc);
    gpio_low(pin_cs);
    spi_write_byte(data);
    gpio_high(pin_cs);
}

/* Envía una palabra de 16 bits como DATO (color RGB565) */
static void lcd_write_data16(u16 data)
{
    gpio_high(pin_dc);
    gpio_low(pin_cs);
    spi_write_byte((u8)(data >> 8));
    spi_write_byte((u8)(data & 0xFFu));
    gpio_high(pin_cs);
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

    n_cmds = *list++;
    while (n_cmds--) {
        cmd        = *list++;
        n_args     = *list++;
        delay_flag = n_args & (u8)ST_CMD_DELAY;
        n_args    &= ~(u8)ST_CMD_DELAY;

        lcd_write_cmd(cmd);
        for (i = 0; i < n_args; i++)
            lcd_write_data(*list++);

        if (delay_flag) {
            delay_ms = *list++;
            if (delay_ms == 255u)
                delay_ms = 500u;
            msleep(delay_ms);
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
    /* Hard reset: HIGH → LOW (10 ms) → HIGH → esperar 200 ms */
    gpio_high(pin_rst);
    msleep(10);
    gpio_low(pin_rst);
    msleep(10);
    gpio_high(pin_rst);
    msleep(200);

    lcd_exec_cmd_list(st7735_rcmd1);      /* Configuración general       */
    lcd_exec_cmd_list(st7735_rcmd2_red);  /* Ventana de dirección 128×160 */
    lcd_exec_cmd_list(st7735_rcmd3);      /* Gamma + encendido            */

    pr_info("chromascope_lcd: display ST7735 inicializado (160×128 px, paisaje)\n");
}

/* ── Primitivas de dibujo ────────────────────────────────────────────────── */

/*
 * lcd_set_window() — define la ventana de escritura (CASET + RASET + RAMWR).
 * Todos los lcd_write_data() posteriores rellenan esa ventana de izquierda
 * a derecha y de arriba a abajo.
 */
static void lcd_set_window(u16 x0, u16 y0, u16 x1, u16 y1)
{
    lcd_write_cmd(ST77XX_CASET);
    lcd_write_data(0x00); lcd_write_data((u8)x0);
    lcd_write_data(0x00); lcd_write_data((u8)x1);

    lcd_write_cmd(ST77XX_RASET);
    lcd_write_data(0x00); lcd_write_data((u8)y0);
    lcd_write_data(0x00); lcd_write_data((u8)y1);

    lcd_write_cmd(ST77XX_RAMWR);
}

/* Rectángulo relleno — optimizado: CS se baja una sola vez para todos los píxeles */
static void lcd_fill_rect(u16 x, u16 y, u16 w, u16 h, u16 color)
{
    u32 total, i;
    u8  hi = (u8)(color >> 8);
    u8  lo = (u8)(color & 0xFFu);

    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w == 0 || h == 0)
        return;
    if (x + w > LCD_WIDTH)  w = (u16)(LCD_WIDTH  - x);
    if (y + h > LCD_HEIGHT) h = (u16)(LCD_HEIGHT - y);

    lcd_set_window(x, y, (u16)(x + w - 1u), (u16)(y + h - 1u));

    total = (u32)w * (u32)h;
    gpio_high(pin_dc);
    gpio_low(pin_cs);
    for (i = 0; i < total; i++) {
        spi_write_byte(hi);
        spi_write_byte(lo);
    }
    gpio_high(pin_cs);
}

static void lcd_draw_pixel(u16 x, u16 y, u16 color)
{
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;
    lcd_set_window(x, y, x, y);
    lcd_write_data16(color);
}

static void lcd_fill_screen(u16 color)
{
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
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

    hue_deg %= 360u;
    sector   = hue_deg / 60u;
    frac     = hue_deg % 60u;      /* fracción dentro del sector [0, 59] */
    x        = frac * 255u / 60u;  /* componente que varía [0, 255]      */

    switch (sector) {
    case 0:  r = 255; g = x;   b = 0;   break;
    case 1:  r = 255u - x; g = 255; b = 0;   break;
    case 2:  r = 0;   g = 255; b = x;   break;
    case 3:  r = 0;   g = 255u - x; b = 255; break;
    case 4:  r = x;   g = 0;   b = 255; break;
    default: r = 255; g = 0;   b = 255u - x; break;
    }

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
    u32 max_count = 1u;
    u32 i, bar_h, x_pos, bar_w;
    u32 n = hist->n_bins;

    if (n == 0 || n > LCD_MAX_BINS)
        n = LCD_MAX_BINS;

    /* 1. Fondo negro */
    lcd_fill_screen(LCD_BLACK);

    /* 2. Encontrar el bin con mayor conteo (para normalizar) */
    for (i = 0; i < n; i++)
        if (hist->bins[i] > max_count)
            max_count = hist->bins[i];

    /* 3. Ancho de cada barra (distribuido uniformemente) */
    bar_w = (u32)LCD_WIDTH / n;
    if (bar_w == 0)
        bar_w = 1u;

    /* 4. Dibujar cada barra */
    for (i = 0; i < n; i++) {
        if (hist->bins[i] == 0)
            continue;

        /* Altura normalizada a LCD_HEIGHT */
        bar_h = (hist->bins[i] * (u32)LCD_HEIGHT) / max_count;
        if (bar_h == 0)
            bar_h = 1u;     /* mínimo visible */

        x_pos = i * (u32)LCD_WIDTH / n;

        lcd_fill_rect(
            (u16)x_pos,
            (u16)((u32)LCD_HEIGHT - bar_h),  /* crece desde abajo */
            (u16)bar_w,
            (u16)bar_h,
            hue_to_rgb565(i * 360u / n)      /* color = matiz del bin */
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
    gpio_set_func(pin_clk,  GPIO_FUNC_OUT);
    gpio_set_func(pin_mosi, GPIO_FUNC_OUT);
    gpio_set_func(pin_cs,   GPIO_FUNC_OUT);
    gpio_set_func(pin_dc,   GPIO_FUNC_OUT);
    gpio_set_func(pin_rst,  GPIO_FUNC_OUT);

    /* Estado de reposo: CS, DC y RST en alto; CLK y MOSI en bajo */
    gpio_high(pin_cs);
    gpio_high(pin_dc);
    gpio_high(pin_rst);
    gpio_low(pin_clk);
    gpio_low(pin_mosi);
}

/* ── module_init / module_exit ───────────────────────────────────────────── */

static int __init chromascope_lcd_init(void)
{
    int ret;

    /* 1. Mapear registros GPIO del BCM2711 */
    gpio_base = ioremap(GPIO_PHYS_BASE, GPIO_MAP_SIZE);
    if (!gpio_base) {
        pr_err("chromascope_lcd: ioremap(0x%08X) falló\n", GPIO_PHYS_BASE);
        return -ENOMEM;
    }

    /* 2. Configurar pines como salida */
    lcd_setup_pins();

    /* 3. Reservar número de dispositivo mayor/menor */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("chromascope_lcd: alloc_chrdev_region falló (%d)\n", ret);
        goto err_unmap;
    }

    /* 4. Registrar dispositivo de caracteres */
    cdev_init(&lcd_cdev, &lcd_fops);
    lcd_cdev.owner = THIS_MODULE;
    ret = cdev_add(&lcd_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("chromascope_lcd: cdev_add falló (%d)\n", ret);
        goto err_unreg;
    }

    /* 5. Crear clase y nodo /dev/chromascope (API kernel >= 6.4) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    lcd_class = class_create(CLASS_NAME);
#else
    lcd_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(lcd_class)) {
        ret = (int)PTR_ERR(lcd_class);
        pr_err("chromascope_lcd: class_create falló (%d)\n", ret);
        goto err_cdev;
    }

    lcd_device = device_create(lcd_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(lcd_device)) {
        ret = (int)PTR_ERR(lcd_device);
        pr_err("chromascope_lcd: device_create falló (%d)\n", ret);
        goto err_class;
    }

    pr_info("chromascope_lcd: driver cargado — /dev/%s (major=%d)\n",
            DEVICE_NAME, MAJOR(dev_num));
    pr_info("chromascope_lcd: CLK=%d MOSI=%d CS=%d DC=%d RST=%d\n",
            pin_clk, pin_mosi, pin_cs, pin_dc, pin_rst);
    pr_info("chromascope_lcd: llama 'ioctl IOCTL_LCD_INIT' para encender la pantalla\n");
    return 0;

    /* Deshacer en orden inverso ante cualquier error */
err_class:
    class_destroy(lcd_class);
err_cdev:
    cdev_del(&lcd_cdev);
err_unreg:
    unregister_chrdev_region(dev_num, 1);
err_unmap:
    iounmap(gpio_base);
    return ret;
}

static void __exit chromascope_lcd_exit(void)
{
    device_destroy(lcd_class, dev_num);
    class_destroy(lcd_class);
    cdev_del(&lcd_cdev);
    unregister_chrdev_region(dev_num, 1);
    iounmap(gpio_base);
    pr_info("chromascope_lcd: driver descargado\n");
}

module_init(chromascope_lcd_init);
module_exit(chromascope_lcd_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ChromaScope Team — CE 4303");
MODULE_DESCRIPTION("Driver ST7735 TFT LCD 128x160 para Raspberry Pi 4 (BCM2711) — SPI por software");
MODULE_VERSION("0.1");
