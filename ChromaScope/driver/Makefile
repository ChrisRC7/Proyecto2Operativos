# Makefile — ChromaScope/driver/
#
# Raspberry Pi 4 (BCM2711) — kernel 6.x
#
# Requisitos previos (una sola vez):
#   sudo apt install raspberrypi-kernel-headers
#
# Targets:
#   make         -> compila ambos modulos (.ko) + test_lcd
#   make modules -> solo los modulos kernel
#   make test    -> solo el binario de prueba userspace
#   make clean   -> limpia todos los artefactos
#   make load    -> insmod chromascope_lcd.ko + verifica /dev/chromascope
#   make unload  -> rmmod chromascope_lcd
#   make run     -> load + ejecutar test_lcd
#   make dmesg   -> ultimos mensajes del kernel relativos a chromascope

KDIR  ?= /lib/modules/$(shell uname -r)/build
PWD   := $(shell pwd)

# Modulos kernel
obj-m += chromascope_lcd.o
obj-m += chromascope_gpio.o

# Programa de prueba userspace
TEST_BIN := test_lcd
TEST_SRC := test_lcd.c

CC     := gcc
CFLAGS := -Wall -Wextra -std=c11

.PHONY: all modules test clean load unload run dmesg

all: modules test

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

test: $(TEST_SRC) chromascope_lcd.h
	$(CC) $(CFLAGS) -o $(TEST_BIN) $(TEST_SRC)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TEST_BIN)

load: modules
	sudo insmod chromascope_lcd.ko
	@ls -la /dev/chromascope && echo "[OK] /dev/chromascope creado"

unload:
	sudo rmmod chromascope_lcd
	@echo "[OK] Driver descargado"

run: load test
	sudo ./$(TEST_BIN)

dmesg:
	sudo dmesg | grep chromascope | tail -20
