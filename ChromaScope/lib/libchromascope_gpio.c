/*
 * libchromascope_gpio.c — Implementación de la biblioteca de usuario GPIO/Servo.
 *
 * Cada función de esta biblioteca:
 *   1. Verifica que el dispositivo esté abierto (fd_gpio >= 0).
 *   2. Construye el argumento correcto para el ioctl correspondiente.
 *   3. Llama a ioctl() sobre /dev/chromascope_gpio.
 *   4. Retorna 0 en éxito o -1 en error (con errno disponible para el llamador).
 *
 * El fd interno (fd_gpio) es un detalle de implementación oculto al usuario.
 * Todo el estado de la sesión con el driver vive aquí, no en el código cliente.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include <stdio.h>      /* fprintf, stderr: mensajes de diagnóstico en caso de error  */
#include <string.h>     /* strerror(): convertir errno en mensaje legible             */
#include <fcntl.h>      /* open(), O_RDWR: abrir el nodo de dispositivo               */
#include <unistd.h>     /* close(): cerrar el file descriptor                         */
#include <sys/ioctl.h>  /* ioctl(): enviar comandos al driver de kernel               */
#include <errno.h>      /* errno: código de error del último syscall fallido          */

/* Header compartido del driver: ioctls, structs gpio_cmd_servo, CHROMASCOPE_DEV. */
#include "../driver/chromascope_gpio.h"

/* Header público de esta biblioteca: declara las funciones que implementamos.    */
#include "libchromascope_gpio.h"

/* ── Estado interno de la biblioteca ──────────────────────────────────────── */

/*
 * fd_gpio — file descriptor del dispositivo /dev/chromascope_gpio.
 *
 * Inicializado a -1 para indicar "no abierto".
 * Todas las funciones de operación verifican que fd_gpio >= 0 antes de
 * llamar a ioctl; si está en -1, retornan -1 de inmediato con errno=ENODEV.
 *
 * Variable estática de módulo: solo es accesible dentro de este archivo .c,
 * lo que garantiza que ningún código externo la manipule accidentalmente.
 */
static int fd_gpio = -1;

/* ── Inicialización y cierre ─────────────────────────────────────────────── */

/*
 * chromascope_gpio_init() — abre el dispositivo y prepara la biblioteca.
 *
 * Internamente llama a open() sobre CHROMASCOPE_DEV (/dev/chromascope_gpio).
 * O_RDWR es obligatorio para poder emitir ioctl de escritura (_IOW / _IOWR).
 */
int chromascope_gpio_init(void)
{
    /* Abrir el nodo de dispositivo del driver GPIO en modo lectura/escritura. */
    fd_gpio = open(CHROMASCOPE_DEV, O_RDWR);
    if (fd_gpio < 0) {
        /* Imprimir el error del sistema operativo para facilitar el diagnóstico.
         * strerror(errno) produce mensajes como "No such file or directory"
         * (driver no cargado) o "Permission denied" (falta sudo/permisos 0666). */
        fprintf(stderr, "[gpio_lib] No se pudo abrir %s: %s\n",
                CHROMASCOPE_DEV, strerror(errno));
        return -1;   /* el llamador sabe que la inicialización falló */
    }
    return 0;   /* dispositivo abierto, listo para recibir ioctls */
}

/*
 * chromascope_gpio_deinit() — cierra el dispositivo y marca la biblioteca como inactiva.
 *
 * Después de esta llamada fd_gpio vuelve a -1, lo que hace que cualquier
 * función posterior retorne -1 sin crashear con un fd inválido.
 */
void chromascope_gpio_deinit(void)
{
    if (fd_gpio >= 0) {
        close(fd_gpio);   /* liberar el file descriptor en el kernel */
        fd_gpio = -1;     /* marcar la biblioteca como "no inicializada" */
    }
}

/* ── Macro auxiliar para ioctls de un solo argumento entero ─────────────── */

/*
 * IOCTL_PIN(cmd, pin): envía un ioctl con un único argumento entero (el pin).
 *
 * Centraliza el patrón: verificar fd → llamar ioctl → reportar error si falla.
 * Usado por gpio_set_output, gpio_set_input, gpio_set_high y gpio_set_low.
 *
 * La macro toma el nombre de la función como `fname` solo para el mensaje
 * de error; no afecta la lógica.
 */
#define IOCTL_PIN(cmd, pin, fname)                                          \
    do {                                                                    \
        int _p = (pin);                                                     \
        /* Verificar que el dispositivo esté abierto antes de llamar ioctl.*/ \
        if (fd_gpio < 0) { errno = ENODEV; return -1; }                    \
        /* ioctl devuelve -1 si hay error; errno contiene el código.       */ \
        if (ioctl(fd_gpio, (cmd), &_p) < 0) {                              \
            fprintf(stderr, "[gpio_lib] " fname "(pin=%d) falló: %s\n",    \
                    _p, strerror(errno));                                   \
            return -1;                                                      \
        }                                                                   \
        return 0;                                                           \
    } while (0)

/* ── Implementaciones de control de pin ─────────────────────────────────── */

/*
 * gpio_set_output() — configura el pin como salida digital (GPFSEL = 001).
 */
int gpio_set_output(int pin)
{
    IOCTL_PIN(IOCTL_GPIO_SET_OUTPUT, pin, "gpio_set_output");
}

/*
 * gpio_set_input() — configura el pin como entrada digital (GPFSEL = 000).
 */
int gpio_set_input(int pin)
{
    IOCTL_PIN(IOCTL_GPIO_SET_INPUT, pin, "gpio_set_input");
}

/*
 * gpio_set_high() — pone el pin en HIGH escribiendo en GPSET0 del BCM2711.
 */
int gpio_set_high(int pin)
{
    IOCTL_PIN(IOCTL_GPIO_SET_HIGH, pin, "gpio_set_high");
}

/*
 * gpio_set_low() — pone el pin en LOW escribiendo en GPCLR0 del BCM2711.
 */
int gpio_set_low(int pin)
{
    IOCTL_PIN(IOCTL_GPIO_SET_LOW, pin, "gpio_set_low");
}

/* ── Lectura de pin ───────────────────────────────────────────────────────── */

/*
 * gpio_read() — lee el nivel lógico actual del pin desde GPLEV0.
 *
 * El ioctl IOCTL_GPIO_READ es _IOWR: tanto escribe (pin) como lee (nivel).
 * El driver usa copy_from_user para leer el pin y copy_to_user para escribir
 * el nivel de vuelta en el mismo int.
 */
int gpio_read(int pin, int *level)
{
    int val;  /* variable que sirve tanto de entrada (pin) como de salida (nivel) */

    /* Verificar estado de la biblioteca antes de cualquier operación. */
    if (fd_gpio < 0) {
        errno = ENODEV;
        return -1;
    }

    /* Verificar que el puntero de salida es válido. */
    if (!level) {
        errno = EINVAL;
        return -1;
    }

    /* Inicializar val con el número de pin.
     * El driver lee 'val' como el pin a consultar, luego sobreescribe 'val'
     * con el nivel leído (0 ó 1) y copia el resultado de vuelta con copy_to_user. */
    val = pin;

    if (ioctl(fd_gpio, IOCTL_GPIO_READ, &val) < 0) {
        fprintf(stderr, "[gpio_lib] gpio_read(pin=%d) falló: %s\n",
                pin, strerror(errno));
        return -1;
    }

    /* Escribir el nivel leído en el puntero de salida del llamador. */
    *level = val;
    return 0;
}

/* ── Control de servo ────────────────────────────────────────────────────── */

/*
 * gpio_servo_move() — genera pulsos PWM en el pin para mover el servo al ángulo.
 *
 * Construye la struct gpio_cmd_servo y la pasa al driver mediante ioctl.
 * La función BLOQUEA durante duration_ms milisegundos mientras el kernel
 * genera los pulsos PWM con udelay() (busy-wait en el kernel).
 *
 * La espera es intencional: el servo SG90 necesita recibir múltiples pulsos
 * consecutivos para alcanzar y mantener la posición. Un solo pulso no mueve
 * el servo de forma confiable.
 */
int gpio_servo_move(int pin, int angle, int duration_ms)
{
    struct gpio_cmd_servo cmd;  /* struct del ioctl IOCTL_GPIO_SET_SERVO_ANGLE */

    /* Verificar que el dispositivo esté abierto. */
    if (fd_gpio < 0) {
        errno = ENODEV;
        return -1;
    }

    /* Rellenar los campos de la struct:
     *   pin         → GPIO BCM donde está conectada la señal del servo
     *   angle       → ángulo destino [0°, 180°]; el driver lo mapea a pulso
     *   duration_ms → ms durante los cuales el driver enviará pulsos repetidos */
    cmd.pin         = pin;
    cmd.angle       = angle;
    cmd.duration_ms = duration_ms;

    /* Enviar el comando al driver. El kernel generará los pulsos PWM de 50 Hz
     * (período de 20 ms) con ancho proporcional al ángulo:
     *   0°   → 1000 µs (1 ms)
     *   90°  → 1500 µs (1.5 ms)
     *   180° → 2000 µs (2 ms)                                                */
    if (ioctl(fd_gpio, IOCTL_GPIO_SET_SERVO_ANGLE, &cmd) < 0) {
        fprintf(stderr, "[gpio_lib] gpio_servo_move(pin=%d, angle=%d, dur=%d) falló: %s\n",
                pin, angle, duration_ms, strerror(errno));
        return -1;
    }

    return 0;
}
