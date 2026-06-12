# Makefile — Bibliotecas estáticas de usuario para ChromaScope.
#
# Produce dos archivos .a (archivos de biblioteca estática):
#   libchromascope_gpio.a  — envuelve el driver /dev/chromascope_gpio
#   libchromascope_lcd.a   — envuelve el driver /dev/chromascope
#
# Las bibliotecas son el ÚNICO punto de acceso al hardware desde el espacio
# de usuario: abstraen todas las llamadas ioctl en funciones de alto nivel.
#
# Uso:
#   make               → compila ambas bibliotecas .a
#   make libgpio       → compila solo libchromascope_gpio.a
#   make liblcd        → compila solo libchromascope_lcd.a
#   make clean         → elimina objetos y archivos .a generados
#
# Para enlazar las bibliotecas con una aplicación:
#   gcc myapp.c -I./lib -L./lib -lchromascope_lcd -lchromascope_gpio -o myapp

# ── Compilador y flags ────────────────────────────────────────────────────

# gcc estándar (no mpicc): las bibliotecas de hardware son código local,
# no necesitan Open MPI.
CC     = gcc

# -Wall -Wextra -Werror: compilación estricta, ninguna advertencia tolerada.
# -std=c11: estándar C 2011 (soporta tipos de ancho fijo, etc.).
# -O2: optimización moderada.
# -fPIC: Position Independent Code — aunque generamos .a (estático), es buena
#        práctica para compatibilidad con posibles .so futuros.
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 -fPIC

# ── Nombres de salida ─────────────────────────────────────────────────────

# Archivo de biblioteca estática para el driver GPIO/Servo.
GPIO_LIB = libchromascope_gpio.a

# Archivo de biblioteca estática para el driver LCD ST7735.
LCD_LIB  = libchromascope_lcd.a

# ── Archivos objeto intermedios ───────────────────────────────────────────

# Objeto compilado de la biblioteca GPIO.
GPIO_OBJ = libchromascope_gpio.o

# Objeto compilado de la biblioteca LCD.
LCD_OBJ  = libchromascope_lcd.o

# ── Targets PHONY ────────────────────────────────────────────────────────
.PHONY: all libgpio liblcd clean

# Target por defecto: compilar ambas bibliotecas.
all: libgpio liblcd

# Alias para construir solo la biblioteca GPIO.
libgpio: $(GPIO_LIB)

# Alias para construir solo la biblioteca LCD.
liblcd: $(LCD_LIB)

# ── Regla de compilación del objeto GPIO ─────────────────────────────────
# -c: compilar a objeto (.o) sin enlazar.
# El objeto encapsula todas las funciones de libchromascope_gpio.c.
$(GPIO_OBJ): libchromascope_gpio.c libchromascope_gpio.h ../driver/chromascope_gpio.h
	$(CC) $(CFLAGS) -c libchromascope_gpio.c -o $(GPIO_OBJ)

# ── Regla de empaquetado del .a GPIO ──────────────────────────────────────
# ar rcs: crear/actualizar el archivo estático.
#   r = insertar (replace) el objeto en el archivo
#   c = crear el archivo si no existe
#   s = crear el índice de símbolos (como ranlib) → evita un paso extra
# El .a resultante puede enlazarse con: gcc ... -L. -lchromascope_gpio
$(GPIO_LIB): $(GPIO_OBJ)
	ar rcs $(GPIO_LIB) $(GPIO_OBJ)
	@echo "Biblioteca creada: $(GPIO_LIB)"

# ── Regla de compilación del objeto LCD ──────────────────────────────────
$(LCD_OBJ): libchromascope_lcd.c libchromascope_lcd.h ../driver/chromascope_lcd.h
	$(CC) $(CFLAGS) -c libchromascope_lcd.c -o $(LCD_OBJ)

# ── Regla de empaquetado del .a LCD ───────────────────────────────────────
$(LCD_LIB): $(LCD_OBJ)
	ar rcs $(LCD_LIB) $(LCD_OBJ)
	@echo "Biblioteca creada: $(LCD_LIB)"

# ── Limpieza ──────────────────────────────────────────────────────────────
# Elimina todos los artefactos generados: objetos .o y archivos .a.
clean:
	rm -f $(GPIO_OBJ) $(GPIO_LIB)
	rm -f $(LCD_OBJ)  $(LCD_LIB)
