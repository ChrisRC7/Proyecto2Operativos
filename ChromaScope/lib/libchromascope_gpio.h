/*
 * libchromascope_gpio.h — Biblioteca de usuario para el driver GPIO/Servo.
 *
 * Esta biblioteca es la ÚNICA capa que interactúa con el driver de kernel
 * chromascope_gpio.ko. Todo acceso al dispositivo /dev/chromascope_gpio debe
 * realizarse a través de las funciones definidas aquí, nunca mediante ioctl
 * directo desde el código de aplicación.
 *
 * Propósito del módulo:
 *   Encapsular las llamadas ioctl al driver en funciones de alto nivel con
 *   nombres semánticos, manejo de errores centralizado y documentación clara.
 *   El driver de kernel solo expone un mecanismo (ioctl); esta biblioteca
 *   expone una política (qué hacer con ese mecanismo).
 *
 * Gestión del dispositivo:
 *   La biblioteca mantiene internamente el file descriptor del dispositivo.
 *   El usuario solo llama a chromascope_gpio_init() al inicio y
 *   chromascope_gpio_deinit() al final; no manipula fds directamente.
 *
 * Uso típico:
 *   if (chromascope_gpio_init() < 0) { ... error ... }
 *   gpio_set_output(18);
 *   gpio_set_high(18);
 *   gpio_servo_move(18, 90, 500);
 *   chromascope_gpio_deinit();
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#ifndef LIBCHROMASCOPE_GPIO_H
#define LIBCHROMASCOPE_GPIO_H

/* ── Inicialización y cierre de la biblioteca ─────────────────────────────── */

/*
 * chromascope_gpio_init() — abre el dispositivo y lo deja listo para operar.
 *
 * Abre /dev/chromascope_gpio y almacena el fd internamente.
 * Debe llamarse UNA sola vez antes de cualquier otra función de esta biblioteca.
 *
 * Retorna:
 *    0  — éxito, el dispositivo está abierto
 *   -1  — error al abrir el dispositivo (errno describe el motivo)
 */
int chromascope_gpio_init(void);

/*
 * chromascope_gpio_deinit() — cierra el dispositivo y libera el fd interno.
 *
 * Debe llamarse cuando la aplicación termina de usar el driver GPIO.
 * Llamar a otras funciones de la biblioteca después de deinit tiene
 * comportamiento indefinido.
 */
void chromascope_gpio_deinit(void);

/* ── Control de función de pin ────────────────────────────────────────────── */

/*
 * gpio_set_output() — configura el pin como salida digital.
 *
 * El driver escribe 001 en los 3 bits de GPFSEL del BCM2711 para el pin.
 * Después de esta llamada el pin puede controlarse con gpio_set_high/low.
 *
 * Parámetros:
 *   pin — número de pin BCM (0–27)
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error de ioctl o dispositivo no abierto
 */
int gpio_set_output(int pin);

/*
 * gpio_set_input() — configura el pin como entrada digital (alta impedancia).
 *
 * El driver escribe 000 en los bits de GPFSEL para el pin.
 * Después de esta llamada se puede leer el nivel con gpio_read().
 *
 * Parámetros:
 *   pin — número de pin BCM (0–27)
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error
 */
int gpio_set_input(int pin);

/* ── Control de nivel de pin ──────────────────────────────────────────────── */

/*
 * gpio_set_high() — pone el pin en nivel lógico HIGH (3.3 V).
 *
 * El driver escribe en el registro GPSET0 del BCM2711.
 * El pin debe estar configurado como salida (gpio_set_output) antes.
 *
 * Parámetros:
 *   pin — número de pin BCM (0–27)
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error
 */
int gpio_set_high(int pin);

/*
 * gpio_set_low() — pone el pin en nivel lógico LOW (0 V).
 *
 * El driver escribe en el registro GPCLR0 del BCM2711.
 *
 * Parámetros:
 *   pin — número de pin BCM (0–27)
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error
 */
int gpio_set_low(int pin);

/*
 * gpio_read() — lee el nivel lógico actual de un pin.
 *
 * El driver lee el registro GPLEV0 del BCM2711.
 * El pin puede estar configurado como entrada o salida.
 *
 * Parámetros:
 *   pin   — número de pin BCM (0–27)
 *   level — puntero donde se escribe el nivel leído (0 ó 1)
 *
 * Retorna:
 *    0  — éxito, *level contiene el nivel del pin
 *   -1  — error
 */
int gpio_read(int pin, int *level);

/* ── Control de servo SG90 ────────────────────────────────────────────────── */

/*
 * gpio_servo_move() — mueve un servo SG90 a un ángulo específico.
 *
 * El driver genera la señal PWM (50 Hz, pulso 1000–2000 µs) en el kernel
 * usando busy-wait (udelay). Esta función bloquea durante duration_ms ms
 * mientras el driver envía los pulsos PWM.
 *
 * Relación pulso-ángulo (servo SG90):
 *   0°   → pulso de 1000 µs
 *   90°  → pulso de 1500 µs
 *   180° → pulso de 2000 µs
 *
 * Parámetros:
 *   pin         — pin BCM de la señal del servo (típicamente 18)
 *   angle       — ángulo destino en grados (0–180)
 *   duration_ms — tiempo en ms durante el cual se envían pulsos PWM
 *
 * Retorna:
 *    0  — éxito
 *   -1  — error
 */
int gpio_servo_move(int pin, int angle, int duration_ms);

#endif /* LIBCHROMASCOPE_GPIO_H */
