// SPDX-License-Identifier: GPL-2.0
/*
 * chromascope_gpio.c — Driver de acceso directo a GPIO para Raspberry Pi 4
 *                      (BCM2711, registros mapeados en memoria física).
 *
 * Accede a los registros GPIO de la BCM2711 directamente con ioremap /
 * ioread32 / iowrite32, sin usar la capa pinctrl/gpiolib del kernel.
 *
 * Dispositivo: /dev/chromascope
 * API: ver chromascope_gpio.h
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
#include <linux/err.h>

#include "chromascope_gpio.h"

/* ── Nombres ─────────────────────────────────────────────────────────────── */
#define DEVICE_NAME  "chromascope"
#define CLASS_NAME   "chromascope"

/* ── Dirección física de los registros GPIO (BCM2711 — RPi 4) ───────────── */
#define BCM2711_GPIO_BASE  0xFE200000U
#define GPIO_MAP_SIZE      0xB4U      /* tamaño mínimo del bloque de registros */

/* ── Offsets de registros (respecto a gpio_base) ─────────────────────────── */
#define GPFSEL0   0x00U   /* Function Select 0  (pines  0–9,  3 bits/pin) */
/* GPFSEL1 = 0x04, GPFSEL2 = 0x08 … se calculan dinámicamente               */
#define GPSET0    0x1CU   /* Pin Output Set 0   (pines  0–31) */
#define GPCLR0    0x28U   /* Pin Output Clear 0 (pines  0–31) */
#define GPLEV0    0x34U   /* Pin Level 0        (pines  0–31) */

/* ── Funciones GPFSEL (3 bits) ───────────────────────────────────────────── */
#define GPIO_FUNC_INPUT   0x0U
#define GPIO_FUNC_OUTPUT  0x1U

/* ── Límite de pines del conector de 40 pines (BCM 0–27) ────────────────── */
#define GPIO_PIN_MAX  27

/* ── Variables globales del módulo ───────────────────────────────────────── */
static void __iomem *gpio_base;
static dev_t          dev_num;
static struct cdev    chromascope_cdev;
static struct class  *chromascope_class;
static struct device *chromascope_device;

static const unsigned int gpio_phys_base = BCM2711_GPIO_BASE;

/* ── Primitivas de bajo nivel ────────────────────────────────────────────── */

/*
 * gpio_set_function() — escribe los 3 bits de función de un pin en GPFSEL.
 *
 *   Cada registro GPFSEL contiene 10 pines × 3 bits = 30 bits útiles.
 *   Registro: GPFSEL0 + (pin/10)*4
 *   Bits:     (pin%10)*3  ..  (pin%10)*3 + 2
 */
static void gpio_set_function(unsigned int pin, unsigned int func)
{
    unsigned int reg   = pin / 10;
    unsigned int shift = (pin % 10) * 3;
    u32 val;

    val  = ioread32(gpio_base + GPFSEL0 + reg * 4U);
    val &= ~(0x7U << shift);
    val |=  (func & 0x7U) << shift;
    iowrite32(val, gpio_base + GPFSEL0 + reg * 4U);
}

static inline void gpio_pin_high(unsigned int pin)
{
    iowrite32(1U << pin, gpio_base + GPSET0);
}

static inline void gpio_pin_low(unsigned int pin)
{
    iowrite32(1U << pin, gpio_base + GPCLR0);
}

static inline int gpio_pin_read(unsigned int pin)
{
    return (int)((ioread32(gpio_base + GPLEV0) >> pin) & 1U);
}

/* ── file_operations ─────────────────────────────────────────────────────── */

static int chromascope_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static int chromascope_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static long chromascope_ioctl(struct file *filp,
                               unsigned int cmd, unsigned long arg)
{
    int pin;
    int val;

    /* Verificar número mágico */
    if (_IOC_TYPE(cmd) != CHROMASCOPE_MAGIC)
        return -ENOTTY;

    /* Todos los comandos reciben/devuelven un entero: leer primero */
    if (copy_from_user(&pin, (int __user *)arg, sizeof(int)))
        return -EFAULT;

    if ((unsigned int)pin > GPIO_PIN_MAX)
        return -EINVAL;

    switch (cmd) {
    case IOCTL_GPIO_SET_OUTPUT:
        gpio_set_function((unsigned int)pin, GPIO_FUNC_OUTPUT);
        pr_debug("chromascope: GPIO %d → OUTPUT\n", pin);
        break;

    case IOCTL_GPIO_SET_INPUT:
        gpio_set_function((unsigned int)pin, GPIO_FUNC_INPUT);
        pr_debug("chromascope: GPIO %d → INPUT\n", pin);
        break;

    case IOCTL_GPIO_SET_HIGH:
        gpio_pin_high((unsigned int)pin);
        pr_debug("chromascope: GPIO %d → HIGH\n", pin);
        break;

    case IOCTL_GPIO_SET_LOW:
        gpio_pin_low((unsigned int)pin);
        pr_debug("chromascope: GPIO %d → LOW\n", pin);
        break;

    case IOCTL_GPIO_READ:
        val = gpio_pin_read((unsigned int)pin);
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            return -EFAULT;
        pr_debug("chromascope: GPIO %d = %d\n", pin, val);
        break;

    default:
        return -ENOTTY;
    }

    return 0;
}

static const struct file_operations chromascope_fops = {
    .owner          = THIS_MODULE,
    .open           = chromascope_open,
    .release        = chromascope_release,
    .unlocked_ioctl = chromascope_ioctl,
};

/* ── Inicialización del módulo ───────────────────────────────────────────── */

static int __init chromascope_init(void)
{
    int ret;

    /* 1. Mapear registros GPIO en espacio virtual del kernel */
    gpio_base = ioremap(gpio_phys_base, GPIO_MAP_SIZE);
    if (!gpio_base) {
        pr_err("chromascope: ioremap() falló para 0x%08X\n", gpio_phys_base);
        return -ENOMEM;
    }
    pr_info("chromascope: registros GPIO mapeados @ phys=0x%08X (BCM2711)\n",
            gpio_phys_base);

    /* 2. Reservar número mayor/menor dinámicamente */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("chromascope: alloc_chrdev_region falló (%d)\n", ret);
        goto err_unmap;
    }

    /* 3. Inicializar y registrar el dispositivo de caracteres */
    cdev_init(&chromascope_cdev, &chromascope_fops);
    chromascope_cdev.owner = THIS_MODULE;

    ret = cdev_add(&chromascope_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("chromascope: cdev_add falló (%d)\n", ret);
        goto err_unreg;
    }

    /* 4. Crear clase en sysfs (kernel >= 6.4 no usa THIS_MODULE) */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    chromascope_class = class_create(CLASS_NAME);
#else
    chromascope_class = class_create(THIS_MODULE, CLASS_NAME);
#endif
    if (IS_ERR(chromascope_class)) {
        ret = (int)PTR_ERR(chromascope_class);
        pr_err("chromascope: class_create falló (%d)\n", ret);
        goto err_cdev;
    }

    /* 5. Crear nodo /dev/chromascope automáticamente */
    chromascope_device = device_create(chromascope_class, NULL,
                                       dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(chromascope_device)) {
        ret = (int)PTR_ERR(chromascope_device);
        pr_err("chromascope: device_create falló (%d)\n", ret);
        goto err_class;
    }

    pr_info("chromascope: driver cargado — major=%d, /dev/%s listo\n",
            MAJOR(dev_num), DEVICE_NAME);
    return 0;

    /* ── Camino de error (orden inverso) ── */
err_class:
    class_destroy(chromascope_class);
err_cdev:
    cdev_del(&chromascope_cdev);
err_unreg:
    unregister_chrdev_region(dev_num, 1);
err_unmap:
    iounmap(gpio_base);
    return ret;
}

/* ── Descarga del módulo ─────────────────────────────────────────────────── */

static void __exit chromascope_exit(void)
{
    device_destroy(chromascope_class, dev_num);
    class_destroy(chromascope_class);
    cdev_del(&chromascope_cdev);
    unregister_chrdev_region(dev_num, 1);
    iounmap(gpio_base);
    pr_info("chromascope: driver descargado\n");
}

module_init(chromascope_init);
module_exit(chromascope_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("ChromaScope Team — CE 4303");
MODULE_DESCRIPTION("ChromaScope — Driver GPIO directo BCM2711 (RPi 4) sin gpiolib");
MODULE_VERSION("0.1");
