/* SPDX-License-Identifier: GPL-2.0
 * chromascope_gpio.h — Definiciones de ioctl compartidas entre
 *                      el módulo kernel y el espacio de usuario.
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#ifndef CHROMASCOPE_GPIO_H
#define CHROMASCOPE_GPIO_H

#ifdef __KERNEL__
#  include <linux/ioctl.h>
#else
#  include <sys/ioctl.h>
#endif

/* Número mágico único para este driver — Raspberry Pi 4 (BCM2711) */
#define CHROMASCOPE_MAGIC  'C'

/* Ruta del dispositivo de caracteres del driver GPIO */
#define CHROMASCOPE_DEV    "/dev/chromascope_gpio"

/*
 * Comandos ioctl
 *
 *  IOCTL_GPIO_SET_OUTPUT — configura un pin como salida
 *      arg: int * → número de pin BCM (0–27)
 *
 *  IOCTL_GPIO_SET_INPUT  — configura un pin como entrada
 *      arg: int * → número de pin BCM (0–27)
 *
 *  IOCTL_GPIO_SET_HIGH   — pone el pin en nivel alto (3.3 V)
 *      arg: int * → número de pin BCM (0–27)
 *
 *  IOCTL_GPIO_SET_LOW    — pone el pin en nivel bajo (0 V)
 *      arg: int * → número de pin BCM (0–27)
 *
 *  IOCTL_GPIO_READ       — lee el nivel actual del pin
 *      arg: int * → entrada: pin BCM; salida: 0 ó 1
 */
#define IOCTL_GPIO_SET_OUTPUT  _IOW(CHROMASCOPE_MAGIC, 1, int)
#define IOCTL_GPIO_SET_INPUT   _IOW(CHROMASCOPE_MAGIC, 2, int)
#define IOCTL_GPIO_SET_HIGH    _IOW(CHROMASCOPE_MAGIC, 3, int)
#define IOCTL_GPIO_SET_LOW     _IOW(CHROMASCOPE_MAGIC, 4, int)
#define IOCTL_GPIO_READ        _IOWR(CHROMASCOPE_MAGIC, 5, int)

struct gpio_cmd_servo {
    int pin;            /* BCM GPIO pin */
    int angle;          /* 0–180 grados */
    int duration_ms;    /* tiempo de pulsado continuo */
};

#define IOCTL_GPIO_SET_SERVO_ANGLE _IOW(CHROMASCOPE_MAGIC, 6, struct gpio_cmd_servo)

#endif /* CHROMASCOPE_GPIO_H */
