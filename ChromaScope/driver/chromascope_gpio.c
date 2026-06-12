// SPDX-License-Identifier: GPL-2.0
/*
 * chromascope_gpio.c — Driver de acceso directo a GPIO para Raspberry Pi 4
 *                      (BCM2711, registros mapeados en memoria física).
 *
 * Este driver evita la capa gpiolib/pinctrl del kernel Linux y accede a los
 * registros GPIO del SoC BCM2711 directamente mediante ioremap + ioread32/iowrite32.
 * El acceso directo es más rápido (sin abstracciones) pero menos portable.
 *
 * Dispositivo de caracteres: /dev/chromascope_gpio
 * API pública:               chromascope_gpio.h (comandos ioctl)
 *
 * Capacidades expuestas al espacio de usuario vía ioctl:
 *   - Configurar un pin como entrada o salida (GPFSEL)
 *   - Poner un pin en nivel alto o bajo (GPSET/GPCLR)
 *   - Leer el nivel lógico de un pin (GPLEV)
 *   - Mover un servo SG90 a un ángulo dado (PWM por software / bit-banging)
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

/* ── Cabeceras del kernel Linux ─────────────────────────────────────────── */
#include <linux/module.h>    /* module_init, module_exit, MODULE_LICENSE, etc.     */
#include <linux/kernel.h>    /* pr_info, pr_err, pr_debug: mensajes al dmesg       */
#include <linux/version.h>   /* LINUX_VERSION_CODE, KERNEL_VERSION: versión kernel */
#include <linux/init.h>      /* __init, __exit: marcar funciones para secciones    */
#include <linux/fs.h>        /* file_operations, alloc/unregister_chrdev_region    */
#include <linux/cdev.h>      /* struct cdev, cdev_init, cdev_add, cdev_del         */
#include <linux/device.h>    /* struct class/device, class_create, device_create   */
#include <linux/uaccess.h>   /* copy_from_user, copy_to_user: transferencia segura */
#include <linux/io.h>        /* ioremap, iounmap, ioread32, iowrite32: MMIO         */
#include <linux/err.h>       /* IS_ERR, PTR_ERR: errores codificados en punteros   */
#include <linux/delay.h>     /* udelay: retardo de microsegundos (activo, no cede) */

#include "chromascope_gpio.h"   /* constantes ioctl, CHROMASCOPE_MAGIC, structs    */

/* ── Nombres del dispositivo y de la clase en sysfs ─────────────────────── */
/* El nombre del dispositivo aparece en /dev/ y en /sys/class/.               */
#define DEVICE_NAME  "chromascope_gpio"   /* nodo: /dev/chromascope_gpio         */
#define CLASS_NAME   "chromascope_gpio"   /* clase: /sys/class/chromascope_gpio/ */

/* ── Dirección física base de los registros GPIO del BCM2711 (RPi 4) ──────── */
/* La memoria del BCM2711 está mapeada a 0xFE000000 (bus address 0x7E000000).  */
/* Los registros GPIO están en el offset 0x200000 desde la base del periférico.*/
#define BCM2711_GPIO_BASE  0xFE200000U

/* Tamaño mínimo del bloque a mapear: cubre el último offset usado (GPLEV0 = 0x34)
 * más algunos bytes de margen. 0xB4 = 180 bytes es suficiente.               */
#define GPIO_MAP_SIZE      0xB4U

/* ── Offsets de registros GPIO respecto a gpio_base ─────────────────────── */
/* Cada registro tiene 32 bits (4 bytes). Acceso con ioread32/iowrite32.       */

/* GPFSEL0 (Function Select 0): controla la función de GPIO 0–9.
 * 3 bits por pin × 10 pines = 30 bits útiles de 32.
 * GPFSEL1 está en GPFSEL0+4 (GPIO 10–19), GPFSEL2 en GPFSEL0+8 (GPIO 20–29).
 * Fórmula: registro = GPFSEL0 + (pin/10)*4; bits = (pin%10)*3 .. (pin%10)*3+2 */
#define GPFSEL0   0x00U

/* GPSET0 (Pin Output Set 0): escribir 1 en el bit N pone GPIO-N en HIGH (3.3V).
 * Escribir 0 en este registro no tiene efecto — no baja el pin.              */
#define GPSET0    0x1CU

/* GPCLR0 (Pin Output Clear 0): escribir 1 en el bit N pone GPIO-N en LOW (0V).
 * Complementario a GPSET0; los dos registros evitan la lectura-modificación-escritura
 * (RMW) que sería necesaria con un registro set/clear unificado.              */
#define GPCLR0    0x28U

/* GPLEV0 (Pin Level 0): leer el bit N devuelve el nivel lógico actual de GPIO-N.
 * Funciona tanto si el pin está en modo INPUT como OUTPUT.                    */
#define GPLEV0    0x34U

/* ── Códigos de función para los bits de GPFSEL (3 bits por pin) ─────────── */
#define GPIO_FUNC_INPUT   0x0U   /* 000 = INPUT  (pin en alta impedancia)       */
#define GPIO_FUNC_OUTPUT  0x1U   /* 001 = OUTPUT (pin manejado por el software)  */
/* 010–111 son funciones alternativas (SPI, I2C, UART, PWM, etc.) — no usadas. */

/* ── Límite de pines del conector J8 de 40 pines ────────────────────────── */
/* El conector expone GPIO 0–27 pero solo usamos 0–18 para el proyecto.        */
#define GPIO_PIN_MAX  18

/* ── Variables globales del módulo ──────────────────────────────────────── */

/* Dirección virtual que mapea los registros físicos (resultado de ioremap).   */
static void __iomem *gpio_base;

/* Número de dispositivo: contiene el major (asignado dinámicamente) y minor=0. */
static dev_t          dev_num;

/* Estructura del dispositivo de caracteres en el VFS del kernel.              */
static struct cdev    chromascope_cdev;

/* Clase en /sys/class/: udev la usa para crear el nodo /dev/chromascope_gpio. */
static struct class  *chromascope_class;

/* Nodo de dispositivo generado por device_create() — referencia para destruirlo.*/
static struct device *chromascope_device;

/*
 * chromascope_gpio_devnode() — callback de udev para permisos del nodo /dev.
 *
 * Asigna permisos 0666 (lectura+escritura para todos) al nodo /dev/chromascope_gpio,
 * evitando tener que ejecutar chmod manualmente después de insmod.
 * Si mode es NULL, udev usa los permisos por defecto del sistema.
 */
static char *chromascope_gpio_devnode(const struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0666;   /* rw-rw-rw-: cualquier usuario puede abrir el dispositivo */
    return NULL;        /* NULL = usar el nombre de clase por defecto para la ruta  */
}

/* Guardar la dirección física para poder referenciarla en los mensajes de log. */
static const unsigned int gpio_phys_base = BCM2711_GPIO_BASE;

/* ── Primitivas de bajo nivel (acceso directo a registros MMIO) ──────────── */

/*
 * gpio_set_function() — configura los 3 bits de función de un pin en GPFSEL.
 *
 * El BCM2711 tiene 6 registros GPFSEL (0–5), cada uno con 10 pines × 3 bits.
 * Para modificar la función de un pin sin tocar los otros del mismo registro:
 *   1. Calcular qué registro GPFSEL contiene el pin (pin / 10).
 *   2. Calcular el desplazamiento en bits dentro del registro ((pin % 10) × 3).
 *   3. Leer el registro actual con ioread32 (lectura atómica de 32 bits MMIO).
 *   4. Limpiar los 3 bits del pin con &= ~(0x7 << shift).
 *   5. Escribir la nueva función con |= (func & 0x7) << shift.
 *   6. Escribir el registro con iowrite32 (escritura atómica de 32 bits MMIO).
 */
static void gpio_set_function(unsigned int pin, unsigned int func)
{
    unsigned int reg   = pin / 10;          /* índice del registro GPFSEL (0–5) */
    unsigned int shift = (pin % 10) * 3;    /* desplazamiento en bits (0,3,6,...27) */
    u32 val;

    /* Leer el registro actual para preservar los bits de los otros pines.     */
    val  = ioread32(gpio_base + GPFSEL0 + reg * 4U);

    /* Limpiar los 3 bits de la función actual del pin.
     * 0x7U = 0b111: máscara de 3 bits. ~(0x7<<shift) invierte para limpiar.  */
    val &= ~(0x7U << shift);

    /* Escribir los 3 bits de la nueva función en la posición del pin.         */
    val |=  (func & 0x7U) << shift;

    /* Escribir el registro modificado de vuelta al hardware.                   */
    iowrite32(val, gpio_base + GPFSEL0 + reg * 4U);
}

/*
 * gpio_pin_high() — pone un pin de salida en nivel alto (3.3 V).
 *
 * Escribe 1 en el bit correspondiente del registro GPSET0.
 * Escribir 0 en GPSET0 no tiene efecto — es un registro "write-1-to-set".
 * La operación es atómica y no requiere leer el registro previamente.
 */
static inline void gpio_pin_high(unsigned int pin)
{
    iowrite32(1U << pin, gpio_base + GPSET0);
}

/*
 * gpio_pin_low() — pone un pin de salida en nivel bajo (0 V).
 *
 * Escribe 1 en el bit correspondiente del registro GPCLR0.
 * Similar a GPSET0 pero para la operación inversa (clear / poner en LOW).
 */
static inline void gpio_pin_low(unsigned int pin)
{
    iowrite32(1U << pin, gpio_base + GPCLR0);
}

/*
 * gpio_pin_read() — lee el nivel lógico actual de un pin (0 ó 1).
 *
 * Lee el registro GPLEV0, desplaza el bit del pin a la posición 0
 * y aplica máscara de 1 bit para obtener solo el valor del pin solicitado.
 */
static inline int gpio_pin_read(unsigned int pin)
{
    return (int)((ioread32(gpio_base + GPLEV0) >> pin) & 1U);
}

/*
 * gpio_servo_move() — mueve un servo SG90 a un ángulo dado usando PWM por software.
 *
 * El protocolo del servo SG90 usa señal PWM de 50 Hz (período = 20 ms):
 *   - Pulso de 1000 µs → posición 0°
 *   - Pulso de 1500 µs → posición 90°
 *   - Pulso de 2000 µs → posición 180°
 *
 * Se interpola linealmente entre 1000 y 2000 µs según el ángulo solicitado.
 * Se envía el número de ciclos PWM calculado de duration_ms / 20 ms.
 *
 * NOTA: udelay() es un retardo activo (busy-wait) que no cede la CPU.
 *       Es aceptable para tiempos cortos (< 1 ms) pero no para retardos largos.
 *       No se puede usar msleep aquí porque estaríamos en un contexto de llamada
 *       del sistema (syscall) que sí puede dormir, pero udelay da menor jitter.
 */
static int gpio_servo_move(unsigned int pin, int angle, int duration_ms)
{
    const unsigned int min_pulse_us = 1000U;  /* pulso para 0°: 1 ms           */
    const unsigned int max_pulse_us = 2000U;  /* pulso para 180°: 2 ms         */
    unsigned int pulse_us;    /* ancho de pulso calculado para el ángulo dado   */
    unsigned int cycles;      /* número de períodos PWM a generar               */

    /* Validar que el ángulo está en el rango físico del servo (0°–180°).      */
    if (angle < 0 || angle > 180)
        return -EINVAL;   /* EINVAL = parámetro inválido                        */

    /* Si no se especificó duración válida, usar 500 ms como valor por defecto. */
    if (duration_ms <= 0)
        duration_ms = 500;

    /* Interpolación lineal: pulse_us = 1000 + angle × (1000/180) µs           */
    pulse_us = min_pulse_us + (unsigned int)angle * (max_pulse_us - min_pulse_us) / 180U;

    /* Número de ciclos PWM completos de 20 ms que caben en duration_ms.       */
    cycles = (unsigned int)duration_ms / 20U;
    if (cycles == 0U)
        cycles = 1U;  /* mínimo un ciclo para que el servo reciba la señal     */

    /* Configurar el pin como salida antes de generar la señal PWM.            */
    gpio_set_function(pin, GPIO_FUNC_OUTPUT);

    /* Generar los ciclos PWM: cada ciclo = pulso HIGH de pulse_us + LOW restante. */
    while (cycles--) {
        gpio_pin_high(pin);             /* inicio del pulso: subir el pin       */
        udelay(pulse_us);               /* mantener HIGH durante pulse_us µs    */
        gpio_pin_low(pin);              /* fin del pulso: bajar el pin          */
        udelay(20000U - pulse_us);      /* esperar el resto del período (20ms - pulso) */
    }

    return 0;   /* éxito                                                        */
}

/* ── Operaciones sobre el archivo del dispositivo (file_operations) ──────── */

/*
 * chromascope_open() — se llama cuando un proceso ejecuta open("/dev/chromascope_gpio").
 * No se necesita ninguna inicialización por apertura; retorna 0 = éxito.
 */
static int chromascope_open(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * chromascope_release() — se llama cuando el proceso cierra el file descriptor.
 * No hay recursos por-fd que liberar; retorna 0 = éxito.
 */
static int chromascope_release(struct inode *inode, struct file *filp)
{
    return 0;
}

/*
 * chromascope_ioctl() — maneja todas las llamadas ioctl desde el espacio de usuario.
 *
 * Es la única interfaz de control del driver. El espacio de usuario llama:
 *   ioctl(fd, IOCTL_GPIO_SET_OUTPUT, &pin_number);
 *
 * El kernel rutea la llamada aquí con:
 *   cmd  = código del comando (codificado con _IO/_IOW/_IOWR en el header)
 *   arg  = argumento genérico (uintptr_t; se castea al tipo apropiado)
 */
static long chromascope_ioctl(struct file *filp,
                               unsigned int cmd, unsigned long arg)
{
    int pin;   /* número del pin BCM para la mayoría de comandos               */
    int val;   /* valor leído (para IOCTL_GPIO_READ)                           */
    int ret;   /* código de retorno de funciones internas                      */

    /* Verificar el número mágico del comando (byte alto de cmd).
     * _IOC_TYPE extrae el número mágico; si no es 'C' (CHROMASCOPE_MAGIC),
     * el comando no es de este driver → ENOTTY = "Not a typewriter" (convención).*/
    if (_IOC_TYPE(cmd) != CHROMASCOPE_MAGIC)
        return -ENOTTY;

    /* Pre-procesamiento: para comandos que reciben un pin, copiarlo del userspace.
     * copy_from_user: copia de forma segura desde la dirección de usuario 'arg'.
     * Retorna el número de bytes NO copiados; 0 = éxito.                      */
    switch (cmd) {
    case IOCTL_GPIO_SET_OUTPUT:
    case IOCTL_GPIO_SET_INPUT:
    case IOCTL_GPIO_SET_HIGH:
    case IOCTL_GPIO_SET_LOW:
    case IOCTL_GPIO_READ:
        if (copy_from_user(&pin, (int __user *)arg, sizeof(int)))
            return -EFAULT;   /* EFAULT = dirección de usuario inválida         */

        /* Rechazar pines fuera del rango permitido (GPIO 0–18).               */
        if ((unsigned int)pin > GPIO_PIN_MAX)
            return -EINVAL;
        break;
    default:
        break;
    }

    /* Despachar el comando al handler correspondiente.                         */
    switch (cmd) {

    case IOCTL_GPIO_SET_OUTPUT:
        /* Configura el pin como salida (GPFSEL: 001 en los 3 bits del pin).   */
        gpio_set_function((unsigned int)pin, GPIO_FUNC_OUTPUT);
        pr_debug("chromascope: GPIO %d → OUTPUT\n", pin);
        break;

    case IOCTL_GPIO_SET_INPUT:
        /* Configura el pin como entrada (GPFSEL: 000 en los 3 bits del pin).  */
        gpio_set_function((unsigned int)pin, GPIO_FUNC_INPUT);
        pr_debug("chromascope: GPIO %d → INPUT\n", pin);
        break;

    case IOCTL_GPIO_SET_HIGH:
        /* Pone el pin en nivel alto (escribe en GPSET0).                      */
        gpio_pin_high((unsigned int)pin);
        pr_debug("chromascope: GPIO %d → HIGH\n", pin);
        break;

    case IOCTL_GPIO_SET_LOW:
        /* Pone el pin en nivel bajo (escribe en GPCLR0).                      */
        gpio_pin_low((unsigned int)pin);
        pr_debug("chromascope: GPIO %d → LOW\n", pin);
        break;

    case IOCTL_GPIO_READ:
        /* Lee el nivel lógico del pin y lo devuelve al espacio de usuario.    */
        val = gpio_pin_read((unsigned int)pin);

        /* copy_to_user: copia de forma segura al buffer del proceso de usuario. */
        if (copy_to_user((int __user *)arg, &val, sizeof(int)))
            return -EFAULT;
        pr_debug("chromascope: GPIO %d = %d\n", pin, val);
        break;

    case IOCTL_GPIO_SET_SERVO_ANGLE: {
        /* Bloque con alcance propio para declarar la variable local servo_cmd. */
        struct gpio_cmd_servo servo_cmd;   /* estructura con pin, ángulo y duración */

        /* Copiar la estructura completa desde el espacio de usuario.           */
        if (copy_from_user(&servo_cmd, (struct gpio_cmd_servo __user *)arg,
                           sizeof(servo_cmd)))
            return -EFAULT;

        /* Verificar que el pin del servo está en rango.                       */
        if ((unsigned int)servo_cmd.pin > GPIO_PIN_MAX)
            return -EINVAL;

        /* Mover el servo al ángulo solicitado durante duration_ms ms.         */
        ret = gpio_servo_move((unsigned int)servo_cmd.pin,
                              servo_cmd.angle,
                              servo_cmd.duration_ms);
        if (ret < 0)
            return ret;   /* propagar el error (ej: -EINVAL si ángulo inválido)*/

        pr_debug("chromascope: servo pin=%d angle=%d duration=%d\n",
                 servo_cmd.pin, servo_cmd.angle, servo_cmd.duration_ms);
        break;
    }

    default:
        return -ENOTTY;   /* comando desconocido                                */
    }

    return 0;   /* éxito                                                        */
}

/*
 * Tabla de operaciones del dispositivo de caracteres.
 * El VFS del kernel llama a estas funciones cuando el espacio de usuario
 * ejecuta open(), close() o ioctl() sobre /dev/chromascope_gpio.
 * THIS_MODULE evita descargar el módulo mientras hay fds abiertos.
 */
static const struct file_operations chromascope_fops = {
    .owner          = THIS_MODULE,       /* bloquear rmmod si el fd está abierto*/
    .open           = chromascope_open,  /* handler de open()                  */
    .release        = chromascope_release, /* handler de close()               */
    .unlocked_ioctl = chromascope_ioctl, /* handler de ioctl() sin el Big Lock */
};

/* ── Inicialización del módulo: se ejecuta al hacer insmod ──────────────── */

static int __init chromascope_init(void)
{
    int ret;

    /* ── Paso 1: Mapear los registros físicos GPIO en el espacio virtual del kernel ── */
    /*
     * ioremap: reserva un rango de direcciones virtuales del kernel y las
     * mapea a la dirección física 0xFE200000 (base de GPIO del BCM2711).
     * Obligatorio porque el kernel no puede acceder directamente a direcciones
     * físicas; todas las acciones deben pasar por la MMU/TLB.
     * Devuelve NULL si no hay espacio virtual disponible.
     */
    gpio_base = ioremap(gpio_phys_base, GPIO_MAP_SIZE);
    if (!gpio_base) {
        pr_err("chromascope: ioremap() falló para 0x%08X\n", gpio_phys_base);
        return -ENOMEM;  /* sin memoria de mapa disponible                     */
    }
    pr_info("chromascope: registros GPIO mapeados @ phys=0x%08X (BCM2711)\n",
            gpio_phys_base);

    /* ── Paso 2: Reservar número de dispositivo mayor y menor ── */
    /*
     * alloc_chrdev_region: pide al kernel que asigne dinámicamente un número
     * mayor (major) para este driver. Es mejor que usar un número fijo
     * para evitar conflictos con otros drivers instalados.
     * Los parámetros: &dev_num (salida), minor_base=0, count=1, name.
     */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("chromascope: alloc_chrdev_region falló (%d)\n", ret);
        goto err_unmap;  /* deshacer el ioremap antes de salir                 */
    }

    /* ── Paso 3: Inicializar y registrar el dispositivo de caracteres ── */
    /*
     * cdev_init: asocia la tabla de file_operations (chromascope_fops) al
     * objeto cdev. Aún no es visible para el VFS.
     * cdev_add: registra el cdev en el VFS con el número mayor/menor asignado.
     * A partir de aquí, un proceso puede abrir el dispositivo si existe el nodo.
     */
    cdev_init(&chromascope_cdev, &chromascope_fops);
    chromascope_cdev.owner = THIS_MODULE;   /* referenciar este módulo          */

    ret = cdev_add(&chromascope_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("chromascope: cdev_add falló (%d)\n", ret);
        goto err_unreg;
    }

    /* ── Paso 4: Crear la clase del dispositivo en sysfs ── */
    /*
     * class_create crea /sys/class/chromascope_gpio/.
     * udev monitorea este directorio y cuando aparece un device dentro,
     * automáticamente crea el nodo /dev/chromascope_gpio.
     *
     * API de class_create cambió en kernel 6.4: eliminó el parámetro THIS_MODULE.
     * Se usa #if para compatibilidad con kernels anteriores y posteriores.
     */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    chromascope_class = class_create(CLASS_NAME);             /* API ≥ 6.4      */
#else
    chromascope_class = class_create(THIS_MODULE, CLASS_NAME); /* API < 6.4    */
#endif
    if (IS_ERR(chromascope_class)) {
        /* IS_ERR/PTR_ERR: el kernel codifica errores como punteros negativos.  */
        ret = (int)PTR_ERR(chromascope_class);
        pr_err("chromascope: class_create falló (%d)\n", ret);
        goto err_cdev;
    }

    /* Asignar el callback de permisos para que udev cree el nodo con 0666.    */
    chromascope_class->devnode = chromascope_gpio_devnode;

    /* ── Paso 5: Crear el nodo /dev/chromascope_gpio ── */
    /*
     * device_create: agrega el dispositivo a la clase y notifica a udev.
     * udev llama a devnode() para los permisos y crea /dev/chromascope_gpio.
     */
    chromascope_device = device_create(chromascope_class, NULL,
                                       dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(chromascope_device)) {
        ret = (int)PTR_ERR(chromascope_device);
        pr_err("chromascope: device_create falló (%d)\n", ret);
        goto err_class;
    }

    pr_info("chromascope: driver cargado — major=%d, /dev/%s listo\n",
            MAJOR(dev_num), DEVICE_NAME);
    return 0;   /* éxito: el driver está completamente inicializado            */

    /* ── Camino de error: deshacer en orden INVERSO al de inicialización ── */
    /* Este patrón "goto encadenado" es el idioma estándar en drivers Linux.   */
err_class:
    class_destroy(chromascope_class);       /* destruir la clase del sysfs     */
err_cdev:
    cdev_del(&chromascope_cdev);            /* quitar el cdev del VFS          */
err_unreg:
    unregister_chrdev_region(dev_num, 1);   /* liberar el número mayor         */
err_unmap:
    iounmap(gpio_base);                     /* desmapear los registros GPIO     */
    return ret;
}

/* ── Descarga del módulo: se ejecuta al hacer rmmod ─────────────────────── */

/*
 * chromascope_exit() — libera todos los recursos en orden inverso a init.
 *
 * El orden correcto es crucial:
 *   1. device_destroy: notifica a udev para eliminar /dev/chromascope_gpio.
 *   2. class_destroy:  elimina /sys/class/chromascope_gpio/.
 *   3. cdev_del:       desregistra el dispositivo del VFS.
 *   4. unregister_chrdev_region: libera el número mayor al pool del kernel.
 *   5. iounmap:        libera el mapeo de memoria virtual → física.
 */
static void __exit chromascope_exit(void)
{
    device_destroy(chromascope_class, dev_num);    /* eliminar /dev/ + udev    */
    class_destroy(chromascope_class);              /* eliminar /sys/class/      */
    cdev_del(&chromascope_cdev);                   /* desregistrar del VFS      */
    unregister_chrdev_region(dev_num, 1);          /* liberar número mayor      */
    iounmap(gpio_base);                            /* desmapear registros GPIO  */
    pr_info("chromascope: driver descargado\n");
}

/* Macros que registran las funciones de init y exit en las secciones
 * .init.text y .exit.text de la imagen del módulo .ko.                         */
module_init(chromascope_init);
module_exit(chromascope_exit);

/* Metadatos del módulo: visibles con `modinfo chromascope_gpio.ko`             */
MODULE_LICENSE("GPL");                      /* licencia: necesaria para usar símbolos GPL del kernel */
MODULE_AUTHOR("ChromaScope Team — CE 4303");
MODULE_DESCRIPTION("ChromaScope — Driver GPIO directo BCM2711 (RPi 4) sin gpiolib");
MODULE_VERSION("0.1");
