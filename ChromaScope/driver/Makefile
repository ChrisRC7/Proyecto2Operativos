# Makefile — driver/chromascope_gpio
#
# Compilar el módulo kernel y el programa de prueba userspace.
# Ejecutar desde la propia Raspberry Pi con los headers instalados.
#
# Raspberry Pi 4 (BCM2711) — kernel 6.x
#
# Requisitos previos (una sola vez):
#   sudo apt install raspberrypi-kernel-headers
#
# Targets:
#   make            → compila módulo + test_blink
#   make module     → solo el módulo .ko
#   make test       → solo el binario de prueba
#   make clean      → limpia todos los artefactos
#   make load       → insmod + verificación de /dev/chromascope
#   make unload     → rmmod
#   make run        → carga el módulo y ejecuta test_blink
#   make dmesg      → muestra los últimos mensajes del kernel

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

# Objeto del módulo kernel
obj-m += chromascope_gpio.o

# Programa de prueba userspace
TEST_BIN := test_blink
TEST_SRC := test_blink.c

CC     := gcc
CFLAGS := -Wall -Wextra -std=c11

# ─────────────────────────────────────────────────────────────────────────────

.PHONY: all module test clean load unload run dmesg

all: module test

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test: $(TEST_SRC) chromascope_gpio.h
	$(CC) $(CFLAGS) -o $(TEST_BIN) $(TEST_SRC)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TEST_BIN)

# ─── Gestión del módulo ───────────────────────────────────────────────────────

load: module
	sudo insmod chromascope_gpio.ko
	@ls -la /dev/chromascope && echo "[OK] /dev/chromascope creado"

unload:
	sudo rmmod chromascope_gpio
	@echo "[OK] Driver descargado"

run: load test
	sudo ./$(TEST_BIN)

dmesg:
	sudo dmesg | grep chromascope | tail -20

