// SPDX-License-Identifier: GPL-2.0
/*
 * chromascope_lcd.c — Driver de kernel para pantalla TFT ST7735 (128×160 px)
 *
 * Hardware objetivo : Raspberry Pi 4 · SoC BCM2711
 * Comunicación      : SPI por software (bit-banging) vía registros GPIO directos
 *                     — sin usar gpiolib ni el subsistema SPI del kernel —
 *
 * El driver implementa todo el protocolo SPI manualmente:
 *   - Manipula las líneas CLK, MOSI, CS, DC y RST bit a bit
 *   - Envía las secuencias de inicialización del controlador ST7735R
 *   - Expone primitivas de dibujo (relleno, píxel, rectángulo, histograma)
 *     al espacio de usuario mediante ioctl sobre /dev/chromascope
 *
 * Pines usados (numeración BCM, configurables como parámetros del módulo):
 *   CLK  → GPIO 26   Reloj SPI (SCLK)
 *   DIN  → GPIO 22   Datos maestro→esclavo (MOSI)
 *   CS   → GPIO 17   Chip Select (activo en LOW)
 *   DC   → GPIO 27   Data/Command (LOW=cmd, HIGH=dato)
 *   RST  → GPIO  4   Reset hardware del ST7735 (activo en LOW)
 *
 * Secuencias de inicialización basadas en la librería Adafruit ST7735
 * (traducidas de C++ a C puro, sin PROGMEM ni herencia de clases).
 *
 * Dispositivo: /dev/chromascope
 * API:         chromascope_lcd.h
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

/* ── Cabeceras del kernel Linux ─────────────────────────────────────────── */

/* module_init, module_exit, MODULE_LICENSE, module_param, MODULE_PARM_DESC:
 * macros fundamentales para declarar y registrar un módulo de kernel.         */
#include <linux/module.h>

/* pr_info(), pr_err(), pr_debug(): funciones de logging del kernel.
 * u8, u16, u32: tipos enteros de ancho exacto definidos por el kernel Linux.  */
#include <linux/kernel.h>

/* LINUX_VERSION_CODE: entero que codifica la versión del kernel en tiempo de
 * compilación (ej: 6.1.21 → 0x060115).
 * KERNEL_VERSION(a,b,c): construye ese entero para comparación con #if.
 * Necesario porque class_create() cambió su firma en el kernel 6.4.           */
#include <linux/version.h>

/* __init: marca la función como "solo necesaria durante la inicialización".
 * El kernel la coloca en la sección .init.text y la descarga de RAM una vez
 * que el módulo termina de cargar — ahorra memoria residente del kernel.
 * __exit: análogo para la función de limpieza (descarte si no hay rmmod).     */
#include <linux/init.h>

/* struct file_operations: tabla de punteros a funciones del dispositivo.
 * alloc_chrdev_region(): reservar dinámicamente un número mayor de dispositivo.
 * unregister_chrdev_region(): liberar el número mayor al descargar.           */
#include <linux/fs.h>

/* struct cdev: representa un dispositivo de caracteres en el VFS del kernel.
 * cdev_init(): inicializar el cdev con una tabla file_operations.
 * cdev_add(): registrar el cdev → el dispositivo se vuelve accesible.
 * cdev_del(): desregistrar el cdev del VFS.                                   */
#include <linux/cdev.h>

/* struct class: agrupación lógica de dispositivos en /sys/class/.
 * struct device: representa el nodo del dispositivo.
 * class_create(): crear la clase → udev puede crear /dev automáticamente.
 * device_create(): agregar el dispositivo → udev crea el nodo /dev/chromascope.
 * device_destroy(), class_destroy(): limpiar al descargar el módulo.          */
#include <linux/device.h>

/* copy_from_user(dst, src_user, n): copia n bytes desde espacio de usuario
 * al kernel de forma segura (verifica que src_user sea válido y accesible).
 * copy_to_user(dst_user, src, n): dirección contraria.
 * Ambas retornan el número de bytes NO copiados; 0 = éxito.
 * Son obligatorias: acceder directamente a punteros de usuario puede causar
 * page faults o accesos a memoria no mapeada dentro del kernel.               */
#include <linux/uaccess.h>

/* ioremap(phys, size): mapea una región de memoria física al espacio virtual
 * del kernel. Imprescindible para acceder a registros de hardware (MMIO).
 * iounmap(virt): libera el mapeo creado por ioremap.
 * ioread32(addr), iowrite32(val, addr): acceso atómico de 32 bits a MMIO.
 * Usan barreras de memoria implícitas para evitar reordenamiento del CPU.     */
#include <linux/io.h>

/* msleep(ms): pone el hilo del kernel a dormir durante ms milisegundos,
 * cediendo el procesador. Solo válido en contexto de proceso (no en interrupts
 * ni softirqs). Preferible a udelay para retardos > 1 ms.                     */
#include <linux/delay.h>

/* DEFINE_MUTEX(name): declara e inicializa un mutex estático.
 * mutex_lock_interruptible(m): adquirir el mutex; retorna -ERESTARTSYS si una
 * señal interrumpe la espera (permite que Ctrl+C funcione correctamente).
 * mutex_unlock(m): liberar el mutex.
 * El mutex protege el bus SPI bit-bang que NO es re-entrante: si dos procesos
 * llamaran a ioctl simultáneamente, las líneas CLK/MOSI quedarían corruptas.  */
#include <linux/mutex.h>

/* IS_ERR(ptr): retorna verdadero si ptr codifica un error (rango [-4095, -1]).
 * PTR_ERR(ptr): extrae el código de error negativo del puntero.
 * Convenio del kernel: ciertas funciones retornan errores como punteros
 * para poder devolver tanto un puntero válido como un código de error.        */
#include <linux/err.h>

/* API pública del driver: constantes de color RGB565, dimensiones del display,
 * structs de los argumentos ioctl y códigos de comando ioctl.                 */
#include "chromascope_lcd.h"

/* ── Nombre del dispositivo y de la clase sysfs ─────────────────────────── */

/* El nombre del nodo de dispositivo: /dev/chromascope                         */
#define DEVICE_NAME  "chromascope"

/* El nombre de la clase en /sys/class/chromascope/ — udev lo monitorea.       */
#define CLASS_NAME   "chromascope"

/* ── Mapa de memoria de los registros GPIO del BCM2711 ──────────────────── */

/* Dirección física base del bloque de registros GPIO en el BCM2711 (RPi 4).
 * La dirección de bus del periférico es 0x7E200000, pero la vista desde la
 * CPU (dirección física) es 0xFE200000 — offset del mapa de periféricos RPi 4.*/
#define GPIO_PHYS_BASE  0xFE200000U

/* Tamaño del bloque MMIO a mapear con ioremap.
 * 0x100 = 256 bytes cubre todos los registros usados hasta GPLEV0 (offset 0x34)
 * con amplio margen para registros adicionales si se necesitan en el futuro.   */
#define GPIO_MAP_SIZE   0x100U

/* GPFSEL0 — GPIO Function Select Register 0 (GPIO 0–9).
 * Cada pin ocupa 3 bits en el registro de 32 bits:
 *   000 = INPUT  (alta impedancia, pin no maneja corriente)
 *   001 = OUTPUT (el software controla el nivel lógico)
 *   010–111 = funciones alternativas (SPI hardware, I2C, UART, PWM, etc.)
 * GPFSEL1 = GPFSEL0 + 4  (GPIO 10–19)
 * GPFSEL2 = GPFSEL0 + 8  (GPIO 20–29)
 * Para el pin N: registro = GPFSEL0 + (N/10)*4, bits = (N%10)*3 .. (N%10)*3+2 */
#define GPFSEL0         0x00U

/* GPSET0 — GPIO Pin Output Set Register 0 (GPIO 0–31).
 * Escribir 1 en el bit N eleva GPIO-N a HIGH (3.3 V lógico).
 * Escribir 0 en este registro NO tiene efecto (registro "set-only").
 * Ventaja: se puede activar un pin sin leer el registro previamente — atómico.*/
#define GPSET0          0x1CU

/* GPCLR0 — GPIO Pin Output Clear Register 0 (GPIO 0–31).
 * Escribir 1 en el bit N baja GPIO-N a LOW (0 V lógico).
 * Igual que GPSET0 pero para la dirección contraria.                          */
#define GPCLR0          0x28U

/* GPLEV0 — GPIO Pin Level Register 0 (GPIO 0–31).
 * Leer el bit N devuelve el nivel lógico actual del GPIO-N (0 ó 1).
 * Funciona tanto si el pin está configurado como INPUT o OUTPUT.              */
#define GPLEV0          0x34U

/* Código de función "001" para los 3 bits de GPFSEL: configura salida digital. */
#define GPIO_FUNC_OUT   0x1U

/* ── Pines SPI y de control — configurables como parámetros del módulo ──── */

/* Valores por defecto de los pines BCM. Se pueden cambiar al cargar el módulo:
 *   sudo insmod chromascope_lcd.ko pin_clk=11 pin_din=10 pin_cs=8 pin_dc=25 pin_rst=27
 * Las variables son `static int` porque son internas al módulo.               */
static int pin_rst  =  4;   /* RST — reset hardware del ST7735 (activo LOW)     */
static int pin_cs   = 17;   /* CS  — chip select: LOW = display seleccionado    */
static int pin_dc   = 27;   /* DC  — data/command: LOW=cmd, HIGH=dato           */
static int pin_din  = 22;   /* DIN — MOSI: datos del maestro (RPi) al esclavo   */
static int pin_clk  = 26;   /* CLK — señal de reloj SPI (SCLK/SCK)             */

/* module_param(variable, tipo, permisos_sysfs):
 * Registra cada variable como parámetro del módulo.
 * 0444 = r--r--r-- : cualquiera puede leerlo en sysfs, nadie puede escribirlo.
 * Los parámetros aparecen en /sys/module/chromascope_lcd/parameters/          */
module_param(pin_clk, int, 0444);
module_param(pin_din, int, 0444);
module_param(pin_cs,  int, 0444);
module_param(pin_dc,  int, 0444);
module_param(pin_rst, int, 0444);

/* MODULE_PARM_DESC: descripción del parámetro, visible con `modinfo` y en sysfs.*/
MODULE_PARM_DESC(pin_clk, "GPIO CLK  (defecto: 26)");
MODULE_PARM_DESC(pin_din, "GPIO MOSI (defecto: 22)");
MODULE_PARM_DESC(pin_cs,  "GPIO CS   (defecto: 17)");
MODULE_PARM_DESC(pin_dc,  "GPIO DC   (defecto: 27)");
MODULE_PARM_DESC(pin_rst, "GPIO RST  (defecto:  4)");

/* ── Variables globales del módulo ──────────────────────────────────────── */

/* Dirección virtual en el kernel que mapea la base física de GPIO (0xFE200000).
 * __iomem: anotación para herramientas de análisis estático (sparse); indica
 * que el puntero apunta a memoria I/O, no a RAM normal.                       */
static void __iomem  *gpio_base;

/* Número de dispositivo que combina major+minor.
 * El major identifica el driver; el minor distingue instancias del mismo driver.
 * Lo asigna dinámicamente alloc_chrdev_region() para evitar conflictos.       */
static dev_t          dev_num;

/* Estructura que representa el dispositivo de caracteres en el VFS del kernel. */
static struct cdev    lcd_cdev;

/* Puntero a la clase del dispositivo en /sys/class/chromascope/.
 * udev monitorea este directorio para crear/destruir nodos /dev automáticamente.*/
static struct class  *lcd_class;

/* Puntero al nodo de dispositivo creado por device_create().
 * Necesario para llamar a device_destroy() al descargar el módulo.            */
static struct device *lcd_device;

/*
 * chromascope_lcd_devnode() — callback de udev para asignar permisos al nodo /dev.
 *
 * udev llama a esta función cuando crea el nodo /dev/chromascope.
 * Parámetros:
 *   dev  — el objeto de dispositivo creado
 *   mode — puntero donde escribir los permisos UNIX del nodo
 *
 * Asignamos 0666 (rw-rw-rw-) para que cualquier usuario (no solo root) pueda
 * abrir el dispositivo desde el espacio de usuario sin necesidad de sudo.
 * Retornar NULL indica que el nombre del nodo es el DEVICE_NAME por defecto.
 */
static char *chromascope_lcd_devnode(const struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0666;   /* permisos: lectura+escritura para dueño, grupo y otros */
    return NULL;        /* usar el nombre predeterminado: /dev/chromascope        */
}

/* Mutex que serializa el acceso al bus SPI bit-bang.
 * DEFINE_MUTEX() declara e inicializa el mutex estáticamente (en tiempo de
 * compilación), más eficiente que mutex_init() en tiempo de ejecución.
 * Necesario porque si dos procesos llaman a ioctl simultáneamente (ej: dos
 * terminales corriendo test_lcd), las líneas SPI se corrompirían.             */
static DEFINE_MUTEX(lcd_mutex);

/* ── Códigos de comando del controlador ST7735 / ST77XX ─────────────────── */

/* Los comandos se envían con DC=LOW seguidos de sus argumentos con DC=HIGH.   */

#define ST77XX_SWRESET  0x01u   /* Software Reset: reinicializa el controlador   */
#define ST77XX_SLPOUT   0x11u   /* Sleep Out: salir del modo de bajo consumo      */
#define ST77XX_NORON    0x13u   /* Normal Display Mode On: modo de visualización normal */
#define ST77XX_INVOFF   0x20u   /* Display Inversion Off: colores normales (no invertidos) */
#define ST77XX_INVON    0x21u   /* Display Inversion On: invertir colores del panel */
#define ST77XX_DISPON   0x29u   /* Display On: encender el panel físicamente       */
#define ST77XX_CASET    0x2Au   /* Column Address Set: definir rango de columnas (eje X) */
#define ST77XX_RASET    0x2Bu   /* Row Address Set: definir rango de filas (eje Y)    */
#define ST77XX_RAMWR    0x2Cu   /* Memory Write: los bytes siguientes son píxeles RGB */
#define ST77XX_MADCTL   0x36u   /* Memory Data Access Control: orientación del GRAM   */
#define ST77XX_COLMOD   0x3Au   /* Interface Pixel Format: profundidad de color (16-bit) */

/* Registros de control de frame rate (FRMCTR = Frame Rate Control).
 * Configuran la frecuencia de refresco en modos normal, idle y partial.
 * Los valores 0x01,0x2C,0x2D corresponden a los recomendados por Adafruit.   */
#define ST7735_FRMCTR1  0xB1u   /* Frame Rate Control (modo normal)               */
#define ST7735_FRMCTR2  0xB2u   /* Frame Rate Control (modo idle/blanqueado)       */
#define ST7735_FRMCTR3  0xB3u   /* Frame Rate Control (modo partial/scroll)        */

/* Inversion Control: controla qué modos activan la inversión de colores.     */
#define ST7735_INVCTR   0xB4u

/* Power Control 1–5: configuran los voltajes internos del panel TFT.
 * Los valores 0xA2,0x02,0x84 son los voltajes AVDD, GVDD y GVCL calibrados   */
#define ST7735_PWCTR1   0xC0u   /* AVDD, GVDD voltage                            */
#define ST7735_PWCTR2   0xC1u   /* VGH, VGL voltage step-up factor               */
#define ST7735_PWCTR3   0xC2u   /* Amplifier current for normal mode             */
#define ST7735_PWCTR4   0xC3u   /* Amplifier current for idle mode               */
#define ST7735_PWCTR5   0xC4u   /* Amplifier current for partial mode            */

/* VCOM Control: ajusta el voltaje de referencia de la línea VCOM.
 * Afecta el contraste y evita el parpadeo (flicker) del panel.               */
#define ST7735_VMCTR1   0xC5u

/* Gamma Correction: ajustan la curva de respuesta de cada canal de color.
 * GMCTRP1: polaridad positiva (transición LOW→HIGH del LCD)
 * GMCTRN1: polaridad negativa (transición HIGH→LOW del LCD)
 * Los 16 bytes de cada tabla son puntos de calibración de la curva gamma.    */
#define ST7735_GMCTRP1  0xE0u
#define ST7735_GMCTRN1  0xE1u

/* ── Bits del registro MADCTL (0x36) — orientación del GRAM ─────────────── */

/* MADCTL controla cómo el controlador ST7735 recorre su GRAM interno al
 * escribir píxeles. Combinando estos bits se logran las 8 orientaciones
 * posibles (normal, espejo, rotado 90°, 180°, 270°, y combinaciones).        */

/* Bit 7: MY — Mirror Y: invierte el orden de escritura de filas (up/down).    */
#define MADCTL_MY   0x80u

/* Bit 6: MX — Mirror X: invierte el orden de escritura de columnas (left/right).*/
#define MADCTL_MX   0x40u

/* Bit 5: MV — Row/Column Exchange: transpone el eje X con el eje Y (rota 90°).
 * Combinado con MY y/o MX produce los cuatro modos de paisaje/retrato.       */
#define MADCTL_MV   0x20u

/* Bit 3: BGR — Blue-Green-Red order: el panel físico del ST7735R tiene los
 * sub-píxeles en orden BGR, no RGB. Sin este bit los rojos y azules se invierten.
 * Con MADCTL_BGR activado, el driver envía datos en orden RGB pero el panel
 * los muestra correctamente porque el controlador hace la conversión interna. */
#define MADCTL_BGR  0x08u

/* ST_CMD_DELAY (bit 7): bandera en el campo n_args de las tablas de init.
 * Si este bit está activo, después de los argumentos del comando hay un byte
 * adicional con el tiempo de espera en ms. Valor 255 significa 500 ms.
 * Este mecanismo permite mezclar comandos y retardos en una sola tabla plana. */
#define ST_CMD_DELAY  0x80u

/* ── Primitivas de acceso directo a los registros GPIO del BCM2711 ────────── */

/*
 * gpio_set_func() — configura los 3 bits de función de un pin en GPFSEL.
 *
 * El BCM2711 tiene 6 registros GPFSEL; cada uno controla 10 pines × 3 bits.
 * Para modificar la función de un pin sin afectar los demás del registro:
 *   1. Calcular qué registro GPFSEL: reg = pin / 10
 *   2. Calcular el desplazamiento de bits: shift = (pin % 10) × 3
 *   3. Leer el registro actual (preservar otros pines)
 *   4. Limpiar los 3 bits del pin: val &= ~(0x7 << shift)
 *   5. Escribir la nueva función:  val |= (func & 0x7) << shift
 *   6. Escribir el registro de vuelta al hardware
 */
static inline void gpio_set_func(int pin, unsigned int func)
{
    /* Calcular índice del registro GPFSEL (0=pines 0-9, 1=pines 10-19, etc.). */
    unsigned int reg   = (unsigned int)pin / 10u;

    /* Calcular el desplazamiento en bits dentro del registro.
     * pin 0 → shift 0, pin 1 → shift 3, pin 9 → shift 27, pin 10 → shift 0 (reg1).*/
    unsigned int shift = ((unsigned int)pin % 10u) * 3u;

    u32 val;  /* valor de 32 bits del registro GPFSEL                          */

    /* Leer el registro GPFSEL correspondiente al pin.
     * ioread32 hace una lectura de 32 bits con barrera de memoria.            */
    val  = ioread32(gpio_base + GPFSEL0 + reg * 4u);

    /* Limpiar los 3 bits de función del pin.
     * 0x7u = 0b111: máscara de 3 bits.
     * ~(0x7u << shift): invertir para tener 0s solo en los bits del pin.     */
    val &= ~(0x7u << shift);

    /* Escribir la nueva función en los 3 bits del pin.
     * (func & 0x7u): asegurar que solo se usen los 3 bits bajos de func.     */
    val |=  (func & 0x7u) << shift;

    /* Escribir el valor modificado de vuelta al registro GPFSEL.              */
    iowrite32(val, gpio_base + GPFSEL0 + reg * 4u);
}

/*
 * gpio_high() — pone el pin especificado en nivel lógico HIGH (3.3 V).
 *
 * Escribe un 1 en el bit correspondiente del registro GPSET0.
 * Esta es una escritura "set-only": escribir 0 no tiene ningún efecto.
 * La operación es no-destructiva: no necesita leer el registro primero.
 */
static inline void gpio_high(int pin)
{
    /* 1u << pin: máscara con un solo bit en la posición del pin.
     * iowrite32 a GPSET0 activa ese GPIO sin afectar los demás pines.        */
    iowrite32(1u << (unsigned int)pin, gpio_base + GPSET0);
}

/*
 * gpio_low() — pone el pin especificado en nivel lógico LOW (0 V).
 *
 * Escribe un 1 en el bit correspondiente del registro GPCLR0.
 * Mismo mecanismo "write-1-to-clear" que GPSET0 pero para la operación inversa.
 */
static inline void gpio_low(int pin)
{
    /* Escribir 1 en el bit del pin en GPCLR0 → ese GPIO va a 0 V.           */
    iowrite32(1u << (unsigned int)pin, gpio_base + GPCLR0);
}

/* ── SPI bit-bang: Modo 0 (CPOL=0, CPHA=0), MSB primero ─────────────────── */

/*
 * spi_write_byte() — envía un byte al ST7735 por SPI usando bit-banging.
 *
 * Modo SPI 0:
 *   CPOL=0: CLK está en LOW cuando el bus está en reposo (idle).
 *   CPHA=0: el dato es muestreado en el FLANCO ASCENDENTE del CLK.
 *
 * MSB first: el bit 7 (más significativo) se envía primero.
 *
 * Diagrama de tiempos para un bit:
 *
 *   MOSI: ─────[DATO ESTABLE]─────────────────
 *   CLK:  ──────────────────┐   ┌────────────
 *                            └───┘
 *                        ↑ ST7735 lee aquí (flanco ascendente)
 *
 * No hay retardos explícitos entre bits: la latencia de iowrite32 (~100 ns
 * en la RPi 4) es suficiente para el ST7735 que acepta hasta 66 MHz.
 */
static void spi_write_byte(u8 byte)
{
    int i;  /* contador del bit actual (7 down to 0)                          */

    /* Iterar desde el bit más significativo (bit 7) hasta el menos (bit 0).  */
    for (i = 7; i >= 0; i--) {

        /* Poner MOSI al nivel del bit actual ANTES de subir el reloj.
         * El ST7735 muestrea MOSI en el flanco ascendente de CLK (CPHA=0),
         * así que el dato debe ser estable antes de que CLK suba.             */
        if (byte & (1u << i))
            gpio_high(pin_din);   /* bit i vale 1: MOSI = HIGH (3.3 V)        */
        else
            gpio_low(pin_din);    /* bit i vale 0: MOSI = LOW (0 V)           */

        /* Generar el flanco ASCENDENTE del reloj: el ST7735 lee MOSI ahora.  */
        gpio_high(pin_clk);

        /* Generar el flanco DESCENDENTE: finaliza el ciclo del bit.
         * CLK vuelve a LOW, listo para el siguiente bit.                      */
        gpio_low(pin_clk);
    }
    /* Al finalizar el bucle: MOSI tiene el valor del bit 0, CLK está en LOW.  */
}

/* ── Funciones de escritura hacia el ST7735 ─────────────────────────────── */

/*
 * lcd_write_cmd() — envía un byte de COMANDO al controlador ST7735.
 *
 * En el protocolo ST7735, la línea DC (Data/Command) distingue los bytes:
 *   DC = LOW  → el byte es un OPCODE de comando (ej: 0x2C = RAMWR)
 *   DC = HIGH → el byte es un argumento o dato de píxel
 *
 * CS (Chip Select) debe estar en LOW durante la transmisión para que el
 * controlador escuche el bus SPI. CS=HIGH desactiva el chip.
 */
static void lcd_write_cmd(u8 cmd)
{
    gpio_low(pin_dc);        /* DC=0: señalizar al ST7735 que viene un comando  */
    gpio_low(pin_cs);        /* CS=0: activar el chip (iniciar la transacción)  */
    spi_write_byte(cmd);     /* enviar el opcode del comando por SPI (8 bits)   */
    gpio_high(pin_cs);       /* CS=1: desactivar el chip (fin de la transacción)*/
}

/*
 * lcd_write_data() — envía un byte de DATO al controlador ST7735.
 *
 * DC=HIGH indica que el byte es:
 *   - Un argumento del último comando enviado (ej: coordenadas de CASET)
 *   - Un byte de color de píxel (dentro de un ciclo RAMWR activo)
 */
static void lcd_write_data(u8 data)
{
    gpio_high(pin_dc);       /* DC=1: señalizar que el byte es un dato/argumento */
    gpio_low(pin_cs);        /* CS=0: activar el chip                            */
    spi_write_byte(data);    /* enviar el byte de dato                           */
    gpio_high(pin_cs);       /* CS=1: desactivar el chip                         */
}

/*
 * lcd_write_data16() — envía un color RGB565 (16 bits) como dos bytes consecutivos.
 *
 * El formato RGB565 empaqueta un color en 16 bits:
 *   Bits 15–11: 5 bits de Rojo   (RRRRR)
 *   Bits 10–5:  6 bits de Verde  (GGGGGG)
 *   Bits  4–0:  5 bits de Azul   (BBBBB)
 *
 * Se envían en big-endian (byte alto primero), que es como el ST7735 los espera.
 *
 * Optimización: CS se baja UNA sola vez para ambos bytes, evitando el overhead
 * de dos transacciones separadas (que implicarían 4 operaciones GPIO extra).
 */
static void lcd_write_data16(u16 data)
{
    gpio_high(pin_dc);                    /* DC=1: modo dato (píxel de color)   */
    gpio_low(pin_cs);                     /* CS=0: inicio de la transacción     */
    spi_write_byte((u8)(data >> 8));      /* byte alto: RRRRRGGG (bits 15–8)   */
    spi_write_byte((u8)(data & 0xFFu));   /* byte bajo: GGGBBBBB (bits  7–0)   */
    gpio_high(pin_cs);                    /* CS=1: fin de la transacción        */
}

/* ── Intérprete de tablas de comandos de inicialización ─────────────────── */

/*
 * lcd_exec_cmd_list() — ejecuta una tabla de comandos ST7735.
 *
 * Formato de la tabla (byte array compacto):
 *
 *   [n_cmds]                     ← número total de comandos en la tabla
 *   Para cada comando:
 *     [opcode]                   ← byte del comando (ej: ST77XX_SWRESET)
 *     [n_args | ST_CMD_DELAY?]   ← número de argumentos; bit 7 = hay retardo
 *     [arg0] [arg1] ... [argN-1] ← argumentos del comando
 *     [delay_ms]                 ← solo si bit 7 estaba activo; 255 = 500 ms
 *
 * Este formato permite codificar toda la secuencia de inicialización del ST7735
 * en arrays de bytes estáticos (como los Rcmd1/2/3 de Adafruit), que son fáciles
 * de modificar y no necesitan lógica condicional compleja.
 */
static void lcd_exec_cmd_list(const u8 *list)
{
    u8  n_cmds;      /* número de comandos restantes en la tabla                */
    u8  cmd;         /* opcode del comando actual                               */
    u8  n_args;      /* número de argumentos (después de limpiar el flag)       */
    u8  delay_flag;  /* guarda si el bit ST_CMD_DELAY estaba activo            */
    u16 delay_ms;    /* valor del retardo en ms (leído de la tabla)             */
    int i;           /* índice para el bucle de argumentos                      */

    /* Leer el primer byte: cuántos comandos tiene esta tabla.
     * El operador `*list++` lee el byte y avanza el puntero al siguiente.     */
    n_cmds = *list++;

    /* Procesar cada comando de la tabla hasta agotar el contador.             */
    while (n_cmds--) {

        cmd    = *list++;   /* leer el opcode del comando y avanzar el puntero  */
        n_args = *list++;   /* leer el byte de n_args (puede tener el flag)     */

        /* Extraer el bit de flag ST_CMD_DELAY (bit 7) del byte n_args.
         * Si está activo, después de los argumentos vendrá un byte de retardo. */
        delay_flag = n_args & (u8)ST_CMD_DELAY;

        /* Limpiar el bit 7 para obtener el número real de argumentos (0–127).  */
        n_args &= ~(u8)ST_CMD_DELAY;

        /* Enviar el opcode del comando al ST7735 con DC=LOW.                   */
        lcd_write_cmd(cmd);

        /* Enviar los argumentos del comando uno por uno con DC=HIGH.
         * El puntero avanza automáticamente con *list++.                       */
        for (i = 0; i < n_args; i++)
            lcd_write_data(*list++);

        /* Si el flag de retardo estaba activo, leer el tiempo y esperar.      */
        if (delay_flag) {
            delay_ms = *list++;           /* byte con el tiempo en ms           */
            if (delay_ms == 255u)
                delay_ms = 500u;          /* valor especial 255 → 500 ms        */
            msleep(delay_ms);             /* dormir cediendo la CPU             */
        }
    }
}

/* ── Tablas de inicialización del ST7735R (panel "Tab Rojo", 128×160) ───── */

/*
 * Estas tablas fueron tomadas de la librería Adafruit_ST7735 (C++ / Arduino)
 * y convertidas a C puro. Cada tabla es un array de u8 con el formato de
 * lcd_exec_cmd_list descrito arriba.
 *
 * La inicialización se divide en tres partes para modularidad:
 *   rcmd1: reset, frame rate, control de potencia, orientación, formato de color
 *   rcmd2: ventana de dirección (tamaño físico del panel)
 *   rcmd3: curvas de corrección gamma + encendido del display
 */

/*
 * st7735_rcmd1[] — Parte 1: configuración base del controlador.
 * Contiene 15 comandos que configuran el comportamiento fundamental del panel.
 */
static const u8 st7735_rcmd1[] = {
    15,                         /* 15 comandos en esta tabla                    */

    /* Cmd 1: Software Reset — reinicia el controlador a valores de fábrica.
     * Necesario incluso después del hard-reset de RST para garantizar un estado
     * consistente. El flag ST_CMD_DELAY indica que después viene un tiempo.
     * 150 ms: tiempo que necesita el ST7735 para completar el reset interno.  */
    ST77XX_SWRESET, ST_CMD_DELAY,
        150,

    /* Cmd 2: Sleep Out — saca el controlador del modo de bajo consumo.
     * El display permanece apagado (sin imagen) pero los circuitos internos
     * se activan. 255 = 500 ms: tiempo que pide la hoja de datos del ST7735.  */
    ST77XX_SLPOUT, ST_CMD_DELAY,
        255,

    /* Cmd 3: Frame Rate Control (modo normal) — 3 argumentos.
     * Controla la frecuencia de refresco del panel en modo de operación normal.
     * 0x01,0x2C,0x2D son los valores recomendados por Adafruit para ~80 Hz.   */
    ST7735_FRMCTR1, 3,
        0x01, 0x2C, 0x2D,

    /* Cmd 4: Frame Rate Control (modo idle) — cuando el consumo está reducido. */
    ST7735_FRMCTR2, 3,
        0x01, 0x2C, 0x2D,

    /* Cmd 5: Frame Rate Control (modo partial/scroll) — 6 argumentos.
     * Los 6 bytes configuran dos conjuntos de parámetros para scroll parcial.  */
    ST7735_FRMCTR3, 6,
        0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,

    /* Cmd 6: Display Inversion Control — 1 argumento.
     * 0x07: los 3 bits bajos controlan la inversión por modo (normal/idle/partial). */
    ST7735_INVCTR, 1,
        0x07,

    /* Cmd 7: Power Control 1 — configura los voltajes AVDD, GVDD, GVCL.
     * 0xA2: AVDD = 4.8V (nivel de voltaje del panel activo)
     * 0x02: Factor de paso-up para VGH/VGL
     * 0x84: modo de configuración del regulador de voltaje                    */
    ST7735_PWCTR1, 3,
        0xA2, 0x02, 0x84,

    /* Cmd 8: Power Control 2 — corriente del amplificador operacional.
     * 0xC5: nivel medio de corriente para equilibrar consumo y velocidad.     */
    ST7735_PWCTR2, 1,
        0xC5,

    /* Cmd 9: Power Control 3 (modo normal) — configura corriente del amplificador. */
    ST7735_PWCTR3, 2,
        0x0A, 0x00,

    /* Cmd 10: Power Control 4 (modo idle). */
    ST7735_PWCTR4, 2,
        0x8A, 0x2A,

    /* Cmd 11: Power Control 5 (modo partial). */
    ST7735_PWCTR5, 2,
        0x8A, 0xEE,

    /* Cmd 12: VCOM Control 1 — ajusta el voltaje VCOM para el contraste.
     * 0x0E: nivel de voltaje VCOM, afecta el brillo y contraste del panel.   */
    ST7735_VMCTR1, 1,
        0x0E,

    /* Cmd 13: Display Inversion Off — mostrar colores normales (no invertidos). */
    ST77XX_INVOFF, 0,

    /* Cmd 14: Memory Access Control — definir la orientación del GRAM.
     * MADCTL_MY | MADCTL_MV = Mirror Y + Row/Col exchange → modo PAISAJE.
     * MADCTL_BGR = el panel tiene subpíxeles en orden BGR físico.
     * Resultado: imagen en orientación 160×128 (horizontal), colores correctos. */
    ST77XX_MADCTL, 1,
        (MADCTL_MY | MADCTL_MV | MADCTL_BGR),

    /* Cmd 15: Interface Pixel Format — formato de color 16 bits (RGB565).
     * 0x05 = 16 bits por píxel (5-6-5). Otros valores: 0x03=12bpp, 0x06=18bpp. */
    ST77XX_COLMOD, 1,
        0x05
};

/*
 * st7735_rcmd2_red[] — Parte 2: ventana de dirección activa.
 * Define el rango de columnas y filas que el controlador considerará válidos.
 * Para el panel de 128×160 en modo paisaje (160 wide × 128 tall):
 *   Columnas: 0x00,0x00 → 0x00,0x9F (0 a 159 = 160 columnas)
 *   Filas:    0x00,0x00 → 0x00,0x7F (0 a 127 = 128 filas)
 * Cada coordenada se envía como 2 bytes porque el ST7735 usa direcciones de 16 bits.
 */
static const u8 st7735_rcmd2_red[] = {
    2,              /* 2 comandos en esta tabla                                  */

    /* Cmd 1: CASET (Column Address Set) — 4 argumentos.
     * Los 4 bytes definen la columna inicial y final:
     *   0x00,0x00 = columna inicial 0x0000 = columna 0
     *   0x00,0x9F = columna final   0x009F = columna 159 (= 160 columnas total) */
    ST77XX_CASET, 4,
        0x00, 0x00,   /* columna inicial: 0                                      */
        0x00, 0x9F,   /* columna final: 159 (0x9F = 159 decimal)                */

    /* Cmd 2: RASET (Row Address Set) — 4 argumentos.
     *   0x00,0x00 = fila inicial 0
     *   0x00,0x7F = fila final   127 (= 128 filas total)                       */
    ST77XX_RASET, 4,
        0x00, 0x00,   /* fila inicial: 0                                         */
        0x00, 0x7F    /* fila final: 127 (0x7F = 127 decimal)                   */
};

/*
 * st7735_rcmd3[] — Parte 3: curvas gamma y encendido final.
 * Las curvas gamma ajustan la respuesta de brillo del panel para que los
 * valores digitales (0–255) se perciban linealmente en el ojo humano.
 * Son 16 puntos de calibración para la curva de polaridad positiva y 16 para negativa.
 */
static const u8 st7735_rcmd3[] = {
    4,              /* 4 comandos en esta tabla                                  */

    /* Cmd 1: Gamma Adjustment Positive — 16 puntos de la curva gamma positiva.
     * Estos valores son los calibrados por Adafruit para el panel "Tab Rojo".  */
    ST7735_GMCTRP1, 16,
        0x02, 0x1C, 0x07, 0x12,
        0x37, 0x32, 0x29, 0x2D,
        0x29, 0x25, 0x2B, 0x39,
        0x00, 0x01, 0x03, 0x10,

    /* Cmd 2: Gamma Adjustment Negative — 16 puntos para la curva negativa.
     * Complementa la curva positiva para reducir el "ghosting" del panel.     */
    ST7735_GMCTRN1, 16,
        0x03, 0x1D, 0x07, 0x06,
        0x2E, 0x2C, 0x29, 0x2D,
        0x2E, 0x2E, 0x37, 0x3F,
        0x00, 0x00, 0x02, 0x10,

    /* Cmd 3: Normal Display Mode On — salir de cualquier modo especial.
     * 10 ms de espera para que el modo normal se aplique completamente.        */
    ST77XX_NORON, ST_CMD_DELAY,
        10,

    /* Cmd 4: Display On — encender físicamente el panel (retroiluminación+LCD).
     * 100 ms de espera recomendados por el datasheet antes de escribir píxeles.*/
    ST77XX_DISPON, ST_CMD_DELAY,
        100
};

/* ── Secuencia de inicialización completa del hardware ──────────────────── */

/*
 * lcd_hw_init() — inicializa el controlador ST7735 desde cero.
 *
 * Esta función se llama cuando el espacio de usuario envía IOCTL_LCD_INIT.
 * Realiza el hard-reset del controlador y le envía las tres tablas de
 * configuración. Después de esta llamada el display está listo para recibir
 * comandos de dibujo (fill, rect, pixel, histogram).
 */
static void lcd_hw_init(void)
{
    /* ── Hard reset por hardware ── */
    /*
     * La secuencia de reset del ST7735 según el datasheet:
     *   RST HIGH → esperar → RST LOW (mínimo 10 µs) → RST HIGH → esperar 120 ms
     * Usamos 10 ms para el pulso LOW (muy conservador) y 200 ms al final
     * para asegurar que el oscilador interno del ST7735 termine de estabilizarse.
     * Sin este reset, el display puede quedar en un estado indefinido si ya
     * estaba encendido (ej: recarga del módulo sin reiniciar la RPi).
     */
    gpio_high(pin_rst);   /* RST=HIGH: estado de reposo (el reset es activo LOW) */
    msleep(10);           /* pausa de estabilización antes del pulso de reset    */
    gpio_low(pin_rst);    /* RST=LOW: activar el reset del controlador ST7735    */
    msleep(10);           /* mantener el reset por 10 ms (suficiente para RST)   */
    gpio_high(pin_rst);   /* RST=HIGH: liberar el reset → controlador arranca    */
    msleep(200);          /* esperar 200 ms: el oscilador interno del ST7735     */
                          /* necesita tiempo para estabilizarse antes de comandos */

    /* ── Enviar las tres tablas de inicialización en orden ── */

    /* Tabla 1: reset SW, frame rate, potencia, orientación MADCTL, formato color. */
    lcd_exec_cmd_list(st7735_rcmd1);

    /* Tabla 2: ventana de dirección activa (CASET 0-159, RASET 0-127).        */
    lcd_exec_cmd_list(st7735_rcmd2_red);

    /* Tabla 3: curvas gamma positiva y negativa + Display On.                  */
    lcd_exec_cmd_list(st7735_rcmd3);

    pr_info("chromascope_lcd: display ST7735 inicializado (160×128 px, paisaje)\n");
}

/* ── Primitivas de dibujo ────────────────────────────────────────────────── */

/*
 * lcd_set_window() — define la ventana activa de escritura de píxeles en el GRAM.
 *
 * El GRAM (Graphic RAM) del ST7735 es el buffer interno de 160×128×2 bytes
 * que almacena la imagen mostrada en pantalla. Para escribir en él:
 *   1. Definir el rectángulo destino con CASET (columnas) y RASET (filas).
 *   2. Emitir RAMWR: los bytes siguientes son píxeles (2 bytes = 1 color RGB565).
 *      El controlador avanza automáticamente la dirección de escritura dentro
 *      del rectángulo: izquierda→derecha, arriba→abajo.
 *
 * Cada coordenada se envía en 2 bytes (big-endian):
 *   Para coord ≤ 255: byte alto = 0x00, byte bajo = coord (suficiente).
 */
static void lcd_set_window(u16 x0, u16 y0, u16 x1, u16 y1)
{
    /* CASET: definir columna inicial (x0) y columna final (x1) */
    lcd_write_cmd(ST77XX_CASET);
    lcd_write_data(0x00);       /* byte alto de x0: siempre 0 para coords < 256 */
    lcd_write_data((u8)x0);     /* byte bajo de x0: columna inicial             */
    lcd_write_data(0x00);       /* byte alto de x1                              */
    lcd_write_data((u8)x1);     /* byte bajo de x1: columna final               */

    /* RASET: definir fila inicial (y0) y fila final (y1) */
    lcd_write_cmd(ST77XX_RASET);
    lcd_write_data(0x00);       /* byte alto de y0                              */
    lcd_write_data((u8)y0);     /* byte bajo de y0: fila inicial                */
    lcd_write_data(0x00);       /* byte alto de y1                              */
    lcd_write_data((u8)y1);     /* byte bajo de y1: fila final                  */

    /* RAMWR: a partir de ahora, los bytes enviados son píxeles de color.
     * El controlador escribe en el GRAM dentro de la ventana definida.        */
    lcd_write_cmd(ST77XX_RAMWR);
}

/*
 * lcd_fill_rect() — dibuja un rectángulo sólido con un color uniforme.
 *
 * Optimización crítica de rendimiento:
 *   CS se baja UNA sola vez al inicio y se sube UNA sola vez al final.
 *   Sin esto, cada píxel requeriría 2 operaciones GPIO extra (CS low/high),
 *   multiplicado por w×h píxeles. Para un rectángulo 160×128, eso sería
 *   40960 operaciones GPIO adicionales innecesarias (~4 ms a 10 MHz).
 *
 * También se pre-calculan los bytes alto y bajo del color fuera del bucle
 * para evitar repetir la misma división por 256 en cada iteración.
 */
static void lcd_fill_rect(u16 x, u16 y, u16 w, u16 h, u16 color)
{
    u32 total;  /* total de píxeles a escribir: w × h                          */
    u32 i;      /* índice del bucle de escritura                                */

    /* Pre-calcular los dos bytes del color RGB565 (evitar división en el bucle) */
    u8 hi = (u8)(color >> 8);       /* bits 15–8: RRRRRGGGG (byte alto)        */
    u8 lo = (u8)(color & 0xFFu);    /* bits  7–0: GGGBBBBB  (byte bajo)        */

    /* Validar: rechazar llamadas con coordenadas fuera de la pantalla.
     * Evita que CASET/RASET reciban valores que el ST7735 no puede procesar.  */
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT || w == 0 || h == 0)
        return;

    /* Recortar el rectángulo al borde derecho de la pantalla si se desborda.  */
    if (x + w > LCD_WIDTH)
        w = (u16)(LCD_WIDTH - x);   /* reducir ancho para no salir del borde   */

    /* Recortar al borde inferior si se desborda.                               */
    if (y + h > LCD_HEIGHT)
        h = (u16)(LCD_HEIGHT - y);  /* reducir alto para no salir del borde    */

    /* Definir la ventana de escritura en el GRAM del controlador.
     * Las coordenadas son inclusivas: (x, y) a (x+w-1, y+h-1).               */
    lcd_set_window(x, y, (u16)(x + w - 1u), (u16)(y + h - 1u));

    /* Calcular el número total de píxeles a escribir.
     * Usar u32 para evitar overflow con rectángulos grandes (ej: 160×128=20480).*/
    total = (u32)w * (u32)h;

    /* Bajar DC=1 (modo dato): todos los bytes que siguen son píxeles de color. */
    gpio_high(pin_dc);

    /* Bajar CS=0 UNA sola vez: todos los píxeles van en una única transacción. */
    gpio_low(pin_cs);

    /* Enviar los dos bytes del color para cada uno de los total píxeles.      */
    for (i = 0; i < total; i++) {
        spi_write_byte(hi);   /* byte alto del color RGB565 (RRRRRGGGG)         */
        spi_write_byte(lo);   /* byte bajo del color RGB565 (GGGBBBBB)          */
    }

    /* Subir CS=1: fin de la transacción masiva de píxeles.                     */
    gpio_high(pin_cs);
}

/*
 * lcd_draw_pixel() — dibuja un único píxel en las coordenadas (x, y).
 *
 * Internamente define una ventana de 1×1 con lcd_set_window y envía
 * el color como dos bytes. Es equivalente a lcd_fill_rect(x, y, 1, 1, color)
 * pero más eficiente al evitar el cálculo de clipping.
 */
static void lcd_draw_pixel(u16 x, u16 y, u16 color)
{
    /* Ignorar el píxel si está fuera de los límites de la pantalla.           */
    if (x >= LCD_WIDTH || y >= LCD_HEIGHT)
        return;

    /* Ventana de 1×1: x0=x1=x, y0=y1=y → un solo píxel.                     */
    lcd_set_window(x, y, x, y);

    /* Enviar el color RGB565 como dos bytes (DC=HIGH automático en lcd_write_data16).*/
    lcd_write_data16(color);
}

/*
 * lcd_fill_screen() — rellena toda la pantalla con un color sólido.
 *
 * Atajo conveniente sobre lcd_fill_rect con la ventana completa.
 * Para 160×128 píxeles × 2 bytes = 40960 bytes en una sola transacción SPI.
 */
static void lcd_fill_screen(u16 color)
{
    /* Llamar fill_rect con el rectángulo completo de la pantalla:
     * origen (0,0), ancho LCD_WIDTH=160, alto LCD_HEIGHT=128.                */
    lcd_fill_rect(0, 0, LCD_WIDTH, LCD_HEIGHT, color);
}

/* ── Conversión de ángulo de matiz (Hue) a color RGB565 ─────────────────── */

/*
 * hue_to_rgb565() — convierte un ángulo de matiz HSV a un color RGB565.
 *
 * Parámetros HSV implícitos:
 *   S = 1.0 (saturación máxima: colores puros, sin blanco mezclado)
 *   V = 1.0 (valor/brillo máximo: colores brillantes, sin negro mezclado)
 *
 * La rueda HSV con S=V=1 tiene 6 sectores de 60° cada uno:
 *   0°–60°:   Rojo → Amarillo (R=255, G sube 0→255, B=0)
 *   60°–120°: Amarillo → Verde (R baja 255→0, G=255, B=0)
 *   120°–180°: Verde → Cian (R=0, G=255, B sube 0→255)
 *   180°–240°: Cian → Azul (R=0, G baja 255→0, B=255)
 *   240°–300°: Azul → Magenta (R sube 0→255, G=0, B=255)
 *   300°–360°: Magenta → Rojo (R=255, G=0, B baja 255→0)
 *
 * Implementado con aritmética entera pura (sin float) para el código de kernel.
 */
static u16 hue_to_rgb565(u32 hue_deg)
{
    u32 sector;   /* sector de 60° al que pertenece el ángulo (0–5)            */
    u32 frac;     /* fracción del ángulo dentro del sector [0°, 59°]           */
    u32 r, g, b;  /* componentes RGB resultantes [0, 255]                      */
    u32 x;        /* componente que varía linealmente dentro del sector        */

    /* Normalizar el ángulo al rango [0°, 359°] usando módulo 360.
     * Maneja valores ≥ 360 sin necesitar un bucle de corrección.             */
    hue_deg %= 360u;

    /* Determinar en qué sector de 60° cae el ángulo:
     *   0° / 60 = sector 0, 61° / 60 = sector 1, ..., 300° / 60 = sector 5  */
    sector = hue_deg / 60u;

    /* Fracción del ángulo dentro del sector: cuántos grados desde el inicio.
     * Ejemplo: hue=95° → sector=1, frac=35° (95-60=35)                       */
    frac = hue_deg % 60u;   /* rango [0, 59]                                   */

    /* Interpolación lineal de la componente variable dentro del sector.
     * Escalar frac de [0,59] a [0,255]: x = frac × 255 / 60.
     * Aritmética entera: se trunca, pero el error es < 1/255 ≈ insignificante. */
    x = frac * 255u / 60u;  /* rango [0, 255]                                  */

    /* Asignar los valores RGB según el sector de 60° en la rueda de colores.  */
    switch (sector) {
    case 0:                                              /* 0°–59°: rojo→amarillo */
        r = 255;       /* R = máximo (rojo puro)                               */
        g = x;         /* G sube de 0 a 255                                    */
        b = 0;         /* B = 0 (sin azul)                                     */
        break;
    case 1:                                              /* 60°–119°: amarillo→verde */
        r = 255u - x;  /* R baja de 255 a 0                                    */
        g = 255;       /* G = máximo (verde puro)                              */
        b = 0;         /* B = 0                                                 */
        break;
    case 2:                                              /* 120°–179°: verde→cian */
        r = 0;         /* R = 0                                                 */
        g = 255;       /* G = máximo                                            */
        b = x;         /* B sube de 0 a 255                                    */
        break;
    case 3:                                              /* 180°–239°: cian→azul */
        r = 0;         /* R = 0                                                 */
        g = 255u - x;  /* G baja de 255 a 0                                    */
        b = 255;       /* B = máximo (azul puro)                               */
        break;
    case 4:                                              /* 240°–299°: azul→magenta */
        r = x;         /* R sube de 0 a 255                                    */
        g = 0;         /* G = 0                                                 */
        b = 255;       /* B = máximo                                            */
        break;
    default:                                             /* 300°–359°: magenta→rojo */
        r = 255;       /* R = máximo (rojo)                                     */
        g = 0;         /* G = 0                                                 */
        b = 255u - x;  /* B baja de 255 a 0                                    */
        break;
    }

    /*
     * Empaquetar los valores R, G, B en formato RGB565 de 16 bits:
     *
     *   Bits 15–11: 5 bits de Rojo   → (b & 0xF8) << 8  (tomar bits 7–3 de B)
     *   Bits 10–5:  6 bits de Verde  → (g & 0xFC) << 3  (tomar bits 7–2 de G)
     *   Bits  4–0:  5 bits de Azul   → (r >> 3)         (tomar bits 7–3 de R)
     *
     * NOTA: en este driver el byte alto codifica Azul y el bajo Rojo debido
     * a cómo el ST7735R con MADCTL_BGR ordena sus subpíxeles internamente.
     * El resultado visual en pantalla es el color correcto del ángulo de matiz.
     */
    return (u16)(
        ((b & 0xF8u) << 8) |   /* 5 bits de Azul  en bits 15–11                */
        ((g & 0xFCu) << 3) |   /* 6 bits de Verde en bits 10–5                 */
        (r >> 3)               /* 5 bits de Rojo  en bits  4–0                 */
    );
}

/* ── Visualización del histograma HSV en la pantalla ─────────────────────── */

/*
 * lcd_show_histogram() — dibuja el histograma HSV como barras verticales de colores.
 *
 * Recibe un array de conteos por bin (un bin = un rango de ángulos de matiz).
 * Cada barra:
 *   - Posición horizontal: distribuida equitativamente en los 160 px del ancho.
 *   - Altura: proporcional al conteo del bin, normalizada al máximo (la barra
 *             más alta ocupa toda la altura LCD_HEIGHT = 128 px).
 *   - Color: el matiz HSV que representa ese bin (bin i → i × 360°/n_bins).
 *
 * Las barras se dibujan desde abajo hacia arriba (origen en la fila inferior).
 */
static void lcd_show_histogram(const struct lcd_cmd_histogram *hist)
{
    u32 max_count = 1u;   /* máximo conteo entre todos los bins.
                           * Iniciar en 1 (no en 0) evita divisiones por cero
                           * si todos los bins están vacíos.                   */
    u32 i;                /* índice del bin actual en el bucle                  */
    u32 bar_h;            /* altura de la barra del bin actual en píxeles       */
    u32 x_pos;            /* posición horizontal (columna) de la barra          */
    u32 bar_w;            /* ancho de cada barra en píxeles                     */
    u32 n = hist->n_bins; /* número de bins solicitado (máx LCD_MAX_BINS = 36)  */

    /* Sanitizar n_bins: si es 0 o excede el máximo soportado, usar el máximo.
     * Esto hace la función robusta ante argumentos inválidos del espacio de usuario.*/
    if (n == 0 || n > LCD_MAX_BINS)
        n = LCD_MAX_BINS;

    /* Paso 1: limpiar la pantalla con fondo negro para empezar desde cero.    */
    lcd_fill_screen(LCD_BLACK);

    /* Paso 2: encontrar el bin con mayor conteo.
     * Este valor se usará como denominador en la normalización de alturas,
     * de modo que la barra más alta siempre toque el tope de la pantalla.     */
    for (i = 0; i < n; i++) {
        if (hist->bins[i] > max_count)
            max_count = hist->bins[i];   /* actualizar el máximo encontrado     */
    }

    /* Paso 3: calcular el ancho de cada barra.
     * Con n=36 bins y LCD_WIDTH=160 px: bar_w = 160/36 = 4 px por barra.
     * Los últimos 16 px del ancho quedan sin usar (hueco al final).           */
    bar_w = (u32)LCD_WIDTH / n;
    if (bar_w == 0)
        bar_w = 1u;   /* mínimo 1 px de ancho para evitar barras invisibles     */

    /* Paso 4: dibujar una barra para cada bin con conteo > 0.                 */
    for (i = 0; i < n; i++) {

        /* Saltar bins vacíos: no hay nada que dibujar, ahorrar ciclos SPI.    */
        if (hist->bins[i] == 0)
            continue;

        /* Calcular la altura proporcional de la barra.
         * Fórmula: bar_h = (conteo_bin / max_conteo) × LCD_HEIGHT
         * Usando aritmética entera: bar_h = bins[i] × LCD_HEIGHT / max_count  */
        bar_h = (hist->bins[i] * (u32)LCD_HEIGHT) / max_count;

        /* Garantizar altura mínima de 1 px: un bin con conteo > 0 siempre
         * debe ser visible, aunque su proporción sea muy pequeña.             */
        if (bar_h == 0)
            bar_h = 1u;

        /* Calcular la posición horizontal: distribuir barras equidistantes.
         * bin 0 → x=0, bin 1 → x=bar_w, ..., bin i → x=i×LCD_WIDTH/n        */
        x_pos = i * (u32)LCD_WIDTH / n;

        /* Dibujar la barra: rectángulo desde la fila (LCD_HEIGHT - bar_h)
         * hasta la fila LCD_HEIGHT - 1 (última fila = parte baja de pantalla). */
        lcd_fill_rect(
            (u16)x_pos,                     /* columna de inicio de la barra   */
            (u16)((u32)LCD_HEIGHT - bar_h), /* fila de inicio (desde abajo)    */
            (u16)bar_w,                     /* ancho de la barra en píxeles    */
            (u16)bar_h,                     /* alto de la barra en píxeles     */
            hue_to_rgb565(i * 360u / n)     /* color = matiz del bin i         */
        );
    }
}

/* ── Operaciones de archivo (VFS → driver) ───────────────────────────────── */

/*
 * lcd_fops_open() — se invoca cuando un proceso ejecuta open("/dev/chromascope").
 *
 * No se necesita ninguna inicialización per-fd; el hardware se inicializa
 * explícitamente mediante IOCTL_LCD_INIT. Retorna 0 = éxito.
 */
static int lcd_fops_open(struct inode *inode, struct file *filp)
{
    return 0;   /* éxito: no hay estado por fd que inicializar                  */
}

/*
 * lcd_fops_release() — se invoca cuando el proceso cierra el file descriptor.
 *
 * No hay recursos por-fd que liberar. El mutex y el hardware permanecen en
 * el estado que los dejó el último ioctl. Retorna 0 = éxito.
 */
static int lcd_fops_release(struct inode *inode, struct file *filp)
{
    return 0;   /* éxito: no hay nada que limpiar por fd                        */
}

/*
 * lcd_fops_ioctl() — dispatcher de comandos ioctl del driver LCD.
 *
 * Es el único punto de entrada de control desde el espacio de usuario.
 * El proceso de usuario llama: ioctl(fd, IOCTL_LCD_*, &argumento)
 *
 * El kernel enruta la syscall ioctl a esta función con:
 *   filp — file descriptor del proceso de usuario
 *   cmd  — código del comando (creado con _IO/_IOW/_IOWR en chromascope_lcd.h)
 *   arg  — argumento genérico (puntero al struct del espacio de usuario)
 */
static long lcd_fops_ioctl(struct file *filp,
                            unsigned int cmd, unsigned long arg)
{
    /* Declarar las structs de argumento de cada posible comando.
     * Están en el stack del kernel; copy_from_user las llenará desde userspace. */
    struct lcd_cmd_fill      fill;   /* IOCTL_LCD_FILL_SCREEN                   */
    struct lcd_cmd_pixel     pixel;  /* IOCTL_LCD_DRAW_PIXEL                    */
    struct lcd_cmd_rect      rect;   /* IOCTL_LCD_DRAW_RECT                     */
    struct lcd_cmd_histogram hist;   /* IOCTL_LCD_SHOW_HISTOGRAM                */
    int ret = 0;                     /* código de retorno (0 = éxito por defecto)*/

    /* Verificar el número mágico del comando: _IOC_TYPE extrae los bits 8–15
     * del entero cmd. Solo aceptamos comandos con magic = 'L' (LCD_MAGIC).
     * ENOTTY ("Not a typewriter") es el error estándar para ioctl desconocido. */
    if (_IOC_TYPE(cmd) != LCD_MAGIC)
        return -ENOTTY;

    /* Adquirir el mutex antes de acceder al bus SPI.
     * mutex_lock_interruptible retorna -ERESTARTSYS si el proceso recibe una
     * señal mientras espera (ej: Ctrl+C). Esto permite que el proceso no quede
     * bloqueado indefinidamente si otro proceso tiene el mutex por mucho tiempo. */
    if (mutex_lock_interruptible(&lcd_mutex))
        return -ERESTARTSYS;

    /* Despachar el comando al handler correspondiente.                         */
    switch (cmd) {

    case IOCTL_LCD_INIT:
        /* Inicializar/reinicializar el hardware ST7735.
         * No necesita argumentos; el cmd no lleva datos (_IO, no _IOW).       */
        lcd_hw_init();
        break;

    case IOCTL_LCD_FILL_SCREEN:
        /* Copiar el struct lcd_cmd_fill desde el espacio de usuario.
         * copy_from_user retorna bytes no copiados; si es != 0, hay un error. */
        if (copy_from_user(&fill, (void __user *)arg, sizeof(fill))) {
            ret = -EFAULT;   /* EFAULT: dirección de usuario inválida           */
            break;
        }
        lcd_fill_screen(fill.color);   /* rellenar toda la pantalla con el color*/
        break;

    case IOCTL_LCD_DRAW_PIXEL:
        /* Copiar el struct lcd_cmd_pixel (x, y, color) desde el usuario.      */
        if (copy_from_user(&pixel, (void __user *)arg, sizeof(pixel))) {
            ret = -EFAULT;
            break;
        }
        lcd_draw_pixel(pixel.x, pixel.y, pixel.color);   /* dibujar el píxel   */
        break;

    case IOCTL_LCD_DRAW_RECT:
        /* Copiar el struct lcd_cmd_rect (x, y, w, h, color) desde el usuario. */
        if (copy_from_user(&rect, (void __user *)arg, sizeof(rect))) {
            ret = -EFAULT;
            break;
        }
        lcd_fill_rect(rect.x, rect.y, rect.w, rect.h, rect.color);
        break;

    case IOCTL_LCD_SHOW_HISTOGRAM:
        /* Copiar el struct lcd_cmd_histogram (bins[], n_bins) desde el usuario.
         * Esta es la struct más grande (~148 bytes); copy_from_user la maneja igual.*/
        if (copy_from_user(&hist, (void __user *)arg, sizeof(hist))) {
            ret = -EFAULT;
            break;
        }
        lcd_show_histogram(&hist);   /* visualizar el histograma en la pantalla */
        break;

    default:
        /* Comando no reconocido: retornar ENOTTY (error estándar para ioctl). */
        ret = -ENOTTY;
    }

    /* Liberar el mutex en cualquier caso (éxito o error).
     * El mutex se libera DESPUÉS de que la operación de hardware terminó,
     * garantizando que el bus SPI no sea interrumpido a mitad de una transferencia.*/
    mutex_unlock(&lcd_mutex);
    return ret;
}

/*
 * lcd_fops — tabla de operaciones del dispositivo de caracteres.
 *
 * El VFS del kernel usa esta tabla para enrutar las syscalls del espacio de
 * usuario a las funciones del driver:
 *   open()  → lcd_fops_open()
 *   close() → lcd_fops_release()
 *   ioctl() → lcd_fops_ioctl()
 *
 * .owner = THIS_MODULE: el módulo no puede descargarse mientras haya procesos
 * con el dispositivo abierto (el kernel incrementa el ref count del módulo).
 *
 * .unlocked_ioctl: versión de ioctl que NO adquiere el Big Kernel Lock (BKL).
 * El BKL fue eliminado en kernel 2.6; todos los drivers modernos usan esta forma.
 */
static const struct file_operations lcd_fops = {
    .owner          = THIS_MODULE,
    .open           = lcd_fops_open,
    .release        = lcd_fops_release,
    .unlocked_ioctl = lcd_fops_ioctl,
};

/* ── Configuración inicial de los pines GPIO ─────────────────────────────── */

/*
 * lcd_setup_pins() — configura todos los pines del bus SPI como salidas digitales
 * y los lleva al estado de reposo (idle) del bus SPI Modo 0.
 *
 * Debe llamarse antes de lcd_hw_init() porque la inicialización del ST7735
 * envía comandos por SPI y necesita los pines ya configurados como salidas.
 */
static void lcd_setup_pins(void)
{
    /* Configurar cada pin como salida digital (GPFSEL: bits 001 para cada pin). */
    gpio_set_func(pin_clk, GPIO_FUNC_OUT);  /* CLK: generar señal de reloj SPI  */
    gpio_set_func(pin_din, GPIO_FUNC_OUT);  /* DIN: enviar datos (MOSI)         */
    gpio_set_func(pin_cs,  GPIO_FUNC_OUT);  /* CS: seleccionar/deseleccionar chip*/
    gpio_set_func(pin_dc,  GPIO_FUNC_OUT);  /* DC: señalizar dato vs comando     */
    gpio_set_func(pin_rst, GPIO_FUNC_OUT);  /* RST: control del reset hardware   */

    /*
     * Llevar el bus SPI al estado de reposo (idle) del Modo 0:
     *
     *   CS  = HIGH: chip deseleccionado → el ST7735 ignora el bus
     *   DC  = HIGH: preparado para dato (se cambia a LOW al enviar comandos)
     *   RST = HIGH: sin reset activo (el reset es activo en LOW)
     *   CLK = LOW:  reposo del reloj en CPOL=0 (clock polarity = 0)
     *   DIN = LOW:  línea de datos en LOW (estado indefinido en idle)
     *
     * Si los pines quedan en un estado incorrecto antes de la inicialización,
     * el ST7735 puede interpretar el ruido en las líneas como comandos.
     */
    gpio_high(pin_cs);    /* CS=1: bus libre, ST7735 no escucha                 */
    gpio_high(pin_dc);    /* DC=1: modo dato (cambiará a 0 al enviar comandos)  */
    gpio_high(pin_rst);   /* RST=1: sin reset (activo LOW → HIGH = normal)      */
    gpio_low(pin_clk);    /* CLK=0: reposo del reloj SPI (CPOL=0)              */
    gpio_low(pin_din);    /* DIN=0: MOSI en LOW mientras el bus está libre      */
}

/* ── Inicialización del módulo: se ejecuta al hacer insmod ──────────────── */

/*
 * chromascope_lcd_init() — registra el driver y configura el hardware.
 *
 * Pasos de inicialización (5 en total):
 *   1. ioremap: mapear los registros físicos GPIO al espacio virtual del kernel.
 *   2. lcd_setup_pins: configurar pines como salidas y llevarlos al estado idle.
 *   3. alloc_chrdev_region: reservar un número mayor dinámicamente.
 *   4. cdev_init + cdev_add: registrar el dispositivo de caracteres en el VFS.
 *   5. class_create + device_create: crear /sys/class/ y /dev/chromascope.
 *
 * Si cualquier paso falla, se deshacen los anteriores en orden inverso
 * usando el patrón idiomático de Linux de `goto encadenado`.
 */
static int __init chromascope_lcd_init(void)
{
    int ret;  /* código de retorno; 0 = éxito                                  */

    /* ── Paso 1: Mapear los registros físicos GPIO en el espacio virtual del kernel ── */
    /*
     * ioremap(dirección_física, tamaño): reserva rango de direcciones virtuales
     * del kernel y configura la MMU para que apunten a 0xFE200000 (GPIO del BCM2711).
     * Es obligatorio: el kernel no puede acceder a direcciones físicas directamente.
     * Retorna NULL si no hay espacio virtual disponible para el mapeo.
     */
    gpio_base = ioremap(GPIO_PHYS_BASE, GPIO_MAP_SIZE);
    if (!gpio_base) {
        pr_err("chromascope_lcd: ioremap(0x%08X) falló\n", GPIO_PHYS_BASE);
        return -ENOMEM;   /* sin espacio de direcciones virtuales disponible    */
    }

    /* ── Paso 2: Configurar los pines GPIO como salidas en estado idle ── */
    /*
     * lcd_setup_pins configura GPFSEL y lleva los pines a su estado de reposo.
     * Debe hacerse ANTES de lcd_hw_init para que el bus SPI esté listo.
     * Si se invirtiera el orden, lcd_hw_init intentaría usar pines con función
     * INPUT (el estado por defecto después del reset), lo que no transmitiría nada.
     */
    lcd_setup_pins();

    /* ── Paso 3: Reservar dinámicamente el número mayor del dispositivo ── */
    /*
     * alloc_chrdev_region(&dev_num, minor_base, count, name):
     *   &dev_num  → donde guardar el número mayor:menor asignado
     *   0         → número menor base (minor=0 para la única instancia)
     *   1         → cantidad de números menores a reservar
     *   DEVICE_NAME → nombre que aparece en /proc/devices
     * El kernel asigna el primer número mayor libre > 0 para evitar conflictos.
     */
    ret = alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME);
    if (ret < 0) {
        pr_err("chromascope_lcd: alloc_chrdev_region falló (%d)\n", ret);
        goto err_unmap;   /* deshacer solo el ioremap antes de salir            */
    }

    /* ── Paso 4: Inicializar y registrar el dispositivo de caracteres ── */
    /*
     * cdev_init(&lcd_cdev, &lcd_fops): asocia la tabla de file_operations al cdev.
     *   Después de esto, lcd_cdev sabe qué función llamar ante open/ioctl/close.
     *
     * lcd_cdev.owner = THIS_MODULE: evita que rmmod descargue el módulo mientras
     *   haya procesos con el device file abierto (el kernel verifica el ref count).
     *
     * cdev_add(&lcd_cdev, dev_num, 1): registra el cdev en el VFS del kernel.
     *   A partir de aquí, el kernel puede enrutar syscalls al driver.
     */
    cdev_init(&lcd_cdev, &lcd_fops);
    lcd_cdev.owner = THIS_MODULE;
    ret = cdev_add(&lcd_cdev, dev_num, 1);
    if (ret < 0) {
        pr_err("chromascope_lcd: cdev_add falló (%d)\n", ret);
        goto err_unreg;   /* deshacer: alloc_chrdev_region                      */
    }

    /* ── Paso 5: Crear la clase en sysfs y el nodo /dev/chromascope ── */
    /*
     * class_create(name): crea /sys/class/chromascope/.
     *   udev (userspace daemon) monitorea /sys/class/ y cuando aparece un
     *   nuevo dispositivo en una clase, automáticamente crea el nodo /dev/.
     *
     * API cambió en kernel 6.4: eliminó el parámetro THIS_MODULE.
     * Se usa #if LINUX_VERSION_CODE para compilar correctamente en ambas versiones.
     *
     * IS_ERR/PTR_ERR: class_create retorna un puntero "errorizado" si falla.
     */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    lcd_class = class_create(CLASS_NAME);              /* API ≥ kernel 6.4       */
#else
    lcd_class = class_create(THIS_MODULE, CLASS_NAME); /* API < kernel 6.4       */
#endif
    if (IS_ERR(lcd_class)) {
        ret = (int)PTR_ERR(lcd_class);   /* extraer el código de error negativo */
        pr_err("chromascope_lcd: class_create falló (%d)\n", ret);
        goto err_cdev;    /* deshacer: cdev_add                                 */
    }

    /* Asignar el callback de permisos: cuando udev cree /dev/chromascope,
     * le asignará permisos 0666 (lectura+escritura para todos los usuarios).   */
    lcd_class->devnode = chromascope_lcd_devnode;

    /* device_create: crea la entrada del dispositivo en /sys/class/chromascope/
     * y notifica a udev para que cree el nodo /dev/chromascope.
     * Parámetros: clase, padre (NULL=ninguno), número de dispositivo, datos
     * privados (NULL), y el nombre de formato (como printf) del nodo.         */
    lcd_device = device_create(lcd_class, NULL, dev_num, NULL, DEVICE_NAME);
    if (IS_ERR(lcd_device)) {
        ret = (int)PTR_ERR(lcd_device);
        pr_err("chromascope_lcd: device_create falló (%d)\n", ret);
        goto err_class;   /* deshacer: class_create                             */
    }

    /* Mensajes de éxito en dmesg (visibles con: sudo dmesg | grep chromascope). */
    pr_info("chromascope_lcd: driver cargado — /dev/%s (major=%d)\n",
            DEVICE_NAME, MAJOR(dev_num));
    pr_info("chromascope_lcd: CLK=%d DIN=%d CS=%d DC=%d RST=%d\n",
            pin_clk, pin_din, pin_cs, pin_dc, pin_rst);
    pr_info("chromascope_lcd: envía IOCTL_LCD_INIT para encender la pantalla\n");
    return 0;   /* éxito: driver completamente cargado, /dev/chromascope listo  */

    /*
     * Camino de error: deshacer en orden INVERSO al de inicialización.
     * Este patrón "goto encadenado" es el idioma estándar de Linux para
     * manejar múltiples recursos en la inicialización de forma segura.
     * Si falla el paso N, se ejecutan las limpiezas de N-1, N-2, ..., 1.
     */
err_class:
    class_destroy(lcd_class);              /* eliminar /sys/class/chromascope/  */
err_cdev:
    cdev_del(&lcd_cdev);                   /* desregistrar el cdev del VFS      */
err_unreg:
    unregister_chrdev_region(dev_num, 1);  /* devolver el número mayor al kernel */
err_unmap:
    iounmap(gpio_base);                    /* liberar el mapeo de memoria MMIO  */
    return ret;                            /* retornar el código de error        */
}

/* ── Descarga del módulo: se ejecuta al hacer rmmod ─────────────────────── */

/*
 * chromascope_lcd_exit() — libera todos los recursos del driver.
 *
 * Se llama cuando el usuario ejecuta: sudo rmmod chromascope_lcd
 * Los recursos se liberan en el ORDEN INVERSO al que fueron creados en _init:
 *   5. device_destroy  ← último en crearse, primero en destruirse
 *   4. class_destroy
 *   3. cdev_del
 *   2. unregister_chrdev_region
 *   1. iounmap         ← primero en crearse, último en destruirse
 *
 * Si el orden se invirtiera (ej: iounmap antes de device_destroy), udev podría
 * intentar acceder al dispositivo mientras los recursos ya están liberados.
 */
static void __exit chromascope_lcd_exit(void)
{
    device_destroy(lcd_class, dev_num);    /* eliminar /dev/chromascope y notificar udev */
    class_destroy(lcd_class);             /* eliminar /sys/class/chromascope/            */
    cdev_del(&lcd_cdev);                  /* desregistrar el cdev del VFS del kernel     */
    unregister_chrdev_region(dev_num, 1); /* devolver el número mayor al pool del kernel */
    iounmap(gpio_base);                   /* liberar el mapeo virtual → físico de GPIO   */
    pr_info("chromascope_lcd: driver descargado\n");
}

/* ── Registro de las funciones de init y exit en el módulo ──────────────── */

/* module_init: registra chromascope_lcd_init() en la sección .init.text del
 * módulo .ko. El kernel la llama al ejecutar insmod o modprobe.               */
module_init(chromascope_lcd_init);

/* module_exit: registra chromascope_lcd_exit() en la sección .exit.text.
 * El kernel la llama al ejecutar rmmod (solo si el ref count del módulo es 0). */
module_exit(chromascope_lcd_exit);

/* ── Metadatos del módulo — visibles con `modinfo chromascope_lcd.ko` ───── */

/* MODULE_LICENSE: declara la licencia del módulo. "GPL" es necesario para
 * poder acceder a símbolos exportados con EXPORT_SYMBOL_GPL del kernel
 * (ej: algunos subsistemas de deferred probing, reguladores, etc.).           */
MODULE_LICENSE("GPL");

/* MODULE_AUTHOR: nombre del autor del módulo.                                  */
MODULE_AUTHOR("ChromaScope Team — CE 4303");

/* MODULE_DESCRIPTION: descripción breve del propósito del módulo.             */
MODULE_DESCRIPTION("Driver ST7735 TFT LCD 128x160 para Raspberry Pi 4 (BCM2711) — SPI por software");

/* MODULE_VERSION: versión del módulo para control de compatibilidad.          */
MODULE_VERSION("0.1");
