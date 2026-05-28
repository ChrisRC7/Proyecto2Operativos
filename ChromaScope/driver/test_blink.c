/*
 * test_blink.c — Prueba básica del driver chromascope_gpio:
 *                parpadea un LED en GPIO 18 cada 3 segundos.
 *
 * Raspberry Pi 4 — Conexión física:
 *   GPIO 18 (pin físico 12) ──[330Ω]── LED (+) ── LED (−) ── GND (pin 14)
 *
 * Compilar: gcc -Wall -o test_blink test_blink.c
 * Ejecutar: sudo ./test_blink
 *
 * ChromaScope · CE 4303 Principios de Sistemas Operativos
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>

#include "chromascope_gpio.h"

/* Pin BCM donde está conectado el LED */
#define LED_PIN  18

/* ── Estado global para el manejador de señales ─────────────────────────── */
static volatile int running = 1;
static int          g_fd    = -1;
static int          g_pin   = LED_PIN;

static void handle_signal(int sig)
{
    (void)sig;
    running = 0;
}

/* ── main ────────────────────────────────────────────────────────────────── */

int main(void)
{
    int state = 0;

    /* Registrar señales para apagar el LED al salir con Ctrl+C */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);

    /* Abrir el dispositivo */
    g_fd = open(CHROMASCOPE_DEV, O_RDWR);
    if (g_fd < 0) {
        perror("open " CHROMASCOPE_DEV);
        fprintf(stderr,
                "¿Está cargado el módulo? → sudo insmod chromascope_gpio.ko\n");
        return EXIT_FAILURE;
    }

    /* Configurar GPIO como salida */
    if (ioctl(g_fd, IOCTL_GPIO_SET_OUTPUT, &g_pin) < 0) {
        perror("ioctl IOCTL_GPIO_SET_OUTPUT");
        close(g_fd);
        return EXIT_FAILURE;
    }
    printf("GPIO %d configurado como salida\n", LED_PIN);
    printf("Parpadeando LED en GPIO %d cada 3 s — Ctrl+C para detener\n\n",
           LED_PIN);

    /* Bucle de parpadeo */
    while (running) {
        if (state) {
            if (ioctl(g_fd, IOCTL_GPIO_SET_HIGH, &g_pin) < 0) {
                perror("ioctl IOCTL_GPIO_SET_HIGH");
                break;
            }
            printf("LED ON\n");
        } else {
            if (ioctl(g_fd, IOCTL_GPIO_SET_LOW, &g_pin) < 0) {
                perror("ioctl IOCTL_GPIO_SET_LOW");
                break;
            }
            printf("LED OFF\n");
        }

        state ^= 1;
        sleep(3);
    }

    /* Apagar LED antes de salir */
    ioctl(g_fd, IOCTL_GPIO_SET_LOW, &g_pin);
    close(g_fd);
    printf("\nLED apagado. Driver sigue cargado.\n");
    return EXIT_SUCCESS;
}
