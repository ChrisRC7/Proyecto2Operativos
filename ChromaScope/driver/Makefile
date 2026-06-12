# Makefile — ChromaScope/driver/
#
# Compila los módulos de kernel (.ko) y el programa de prueba userspace.
#
# Hardware objetivo: Raspberry Pi 4 (BCM2711) con kernel 6.x
#
# Prerrequisito (una sola vez):
#   sudo apt install raspberrypi-kernel-headers
#   (instala las cabeceras del kernel necesarias para compilar módulos)
#
# Targets disponibles:
#   make         → compila ambos módulos (.ko) + test_lcd
#   make modules → solo los módulos del kernel
#   make test    → solo el binario de prueba en espacio de usuario
#   make clean   → elimina todos los artefactos de compilación
#   make load    → carga ambos módulos con insmod
#   make unload  → descarga ambos módulos con rmmod
#   make run     → load + ejecutar test_lcd
#   make dmesg   → mostrar los últimos mensajes del kernel de chromascope

# ── Directorio del árbol de construcción del kernel ──────────────────────
# ?= asigna solo si KDIR no está ya definida (permite override desde la CLI).
# uname -r devuelve la versión del kernel en ejecución (ej: "6.1.21-v8+").
# /lib/modules/$(uname -r)/build es el enlace simbólico al árbol del kernel.
KDIR  ?= /lib/modules/$(shell uname -r)/build

# Directorio actual: necesario para que el Makefile del kernel sepa dónde
# están los fuentes del módulo (se pasa como M=$(PWD)).
PWD   := $(shell pwd)

# ── Módulos kernel a compilar ────────────────────────────────────────────
# obj-m += <nombre>.o: indica al sistema de build del kernel que compile
# <nombre>.c y produzca el módulo <nombre>.ko.
# Los dos módulos son independientes y se cargan/descargan por separado.
obj-m += chromascope_lcd.o    # driver de la pantalla TFT ST7735 128×160
obj-m += chromascope_gpio.o   # driver GPIO directo BCM2711 + servo SG90

# ── Binario de prueba en espacio de usuario ───────────────────────────────
TEST_BIN := test_lcd      # nombre del ejecutable de prueba
TEST_SRC := test_lcd.c    # fuente del programa de prueba

# Compilador C para el programa de userspace (no para el módulo kernel)
CC     := gcc
# -Wall -Wextra: advertencias estándar y extras
# -std=c11: estándar C 2011
CFLAGS := -Wall -Wextra -std=c11

# ── Targets PHONY (no corresponden a archivos físicos) ───────────────────
.PHONY: all modules test clean load unload run dmesg

# Target por defecto: compilar módulos del kernel Y el programa de prueba
all: modules test

# ── Compilar los módulos del kernel ──────────────────────────────────────
# $(MAKE) -C $(KDIR): invocar el make del árbol del kernel con su Makefile
# M=$(PWD): indicarle dónde están los fuentes del módulo externo
# modules: target estándar del kernel para compilar módulos externos
modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

# ── Compilar el programa de prueba userspace ─────────────────────────────
# Depende de test_lcd.c y de ambos headers para detectar cambios en la API.
test: $(TEST_SRC) chromascope_lcd.h chromascope_gpio.h
	$(CC) $(CFLAGS) -o $(TEST_BIN) $(TEST_SRC)

# ── Limpiar todos los artefactos generados ───────────────────────────────
# $(MAKE) -C $(KDIR) M=$(PWD) clean: limpia los .o, .ko, .mod.c del módulo
# rm -f $(TEST_BIN): elimina el ejecutable de prueba
clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TEST_BIN)

# ── Cargar los módulos en el kernel ──────────────────────────────────────
# El orden importa: gpio debe cargarse primero porque el LCD puede usarlo.
# insmod carga el .ko desde el directorio actual (no busca en /lib/modules).
# @ls: verificar que los nodos /dev se crearon (udev los crea automáticamente
#      gracias a device_create() + la clase definida en cada driver).
load: modules
	sudo insmod chromascope_gpio.ko
	sudo insmod chromascope_lcd.ko
	@ls -la /dev/chromascope_gpio && echo "[OK] /dev/chromascope_gpio creado"
	@ls -la /dev/chromascope      && echo "[OK] /dev/chromascope creado"

# ── Descargar los módulos del kernel ─────────────────────────────────────
# El orden inverso a la carga: primero LCD (que puede depender de GPIO),
# luego GPIO. rmmod falla si hay algún proceso con el dispositivo abierto.
unload:
	sudo rmmod chromascope_lcd
	sudo rmmod chromascope_gpio
	@echo "[OK] Ambos drivers descargados"

# ── Cargar y ejecutar el programa de prueba ──────────────────────────────
# Depende de load (que a su vez depende de modules) y de test.
# sudo es necesario porque el acceso a /dev/chromascope requiere permisos.
run: load test
	sudo ./$(TEST_BIN)

# ── Ver mensajes del kernel relevantes ───────────────────────────────────
# dmesg: buffer de mensajes del kernel (pr_info, pr_err del driver)
# grep chromascope: filtrar solo los del driver
# tail -20: mostrar los últimos 20 mensajes
dmesg:
	sudo dmesg | grep chromascope | tail -20
