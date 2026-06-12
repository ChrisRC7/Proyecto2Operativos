# Makefile — ChromaScope: sistema de procesamiento distribuido de imágenes.
#
# Produce tres binarios independientes:
#   chromascope_mpi  — programa principal MPI (master + workers)
#   image_test       — prueba standalone del módulo de imagen
#   crypto_test      — prueba standalone del cifrador ChaCha20
#
# Uso:
#   make             → compila los tres binarios
#   make mpi         → solo el binario MPI
#   make run-local   → compila + ejecuta localmente con 4 procesos
#   make test-image  → compila + ejecuta la prueba de imagen
#   make test-crypto → compila + ejecuta la prueba de ChaCha20
#   make clean       → elimina todos los binarios y archivos generados

# ── Compiladores ─────────────────────────────────────────────────────────
# gcc:   compilador C estándar para los tests standalone
CC     = gcc
# mpicc: wrapper de GCC que añade automáticamente los flags de inclusión
#        y enlazado de la librería Open MPI (equivale a: gcc + -I/mpi/include -lmpi)
MPICC  = mpicc

# ── Flags de compilación comunes ─────────────────────────────────────────
# -Wall:    habilitar todas las advertencias estándar
# -Wextra:  advertencias adicionales (parámetros no usados, comparaciones dudosas)
# -Werror:  tratar advertencias como errores → el código no compila con warnings
# -std=c11: usar el estándar ISO C 2011 (soporta _Static_assert, atomics, etc.)
# -O2:      optimización de nivel 2 (equilibrio entre velocidad y tamaño binario)
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2

# ── Nombres de los binarios de salida ────────────────────────────────────
MPI_TARGET        = chromascope_mpi   # binario MPI principal
IMAGE_TEST_TARGET = image_test        # test de carga/guardado de imágenes PPM
CRYPTO_TEST_TARGET = crypto_test      # test del cifrador ChaCha20

# ── Archivos fuente del binario MPI ──────────────────────────────────────
# Incluye el master, worker, monitor, módulos de imagen y criptografía.
MPI_SRC = \
	Server/main_server.c \
	Nodes/worker.c \
	Nodes/resource_monitor.c \
	Image/ppm.c \
	Image/histogram.c \
	Image/rgb_filter.c \
	Crypto/chacha20.c

# ── Archivos fuente del test de imagen ───────────────────────────────────
IMAGE_SRC = \
	Image/ppm.c \
	Image/histogram.c \
	Image/image_test.c \
	Image/rgb_filter.c

# ── Archivos fuente del test criptográfico ───────────────────────────────
CRYPTO_SRC = \
	Crypto/chacha20.c \
	Crypto/crypto_test.c

# ── Targets PHONY (no generan archivos con ese nombre) ───────────────────
.PHONY: all clean mpi image crypto run-local test-image test-crypto

# Target por defecto: compilar los tres binarios
all: mpi image crypto

# Alias para compilar solo el binario MPI
mpi: $(MPI_TARGET)

# Alias para compilar solo el test de imagen
image: $(IMAGE_TEST_TARGET)

# Alias para compilar solo el test criptográfico
crypto: $(CRYPTO_TEST_TARGET)

# ── Regla de compilación del binario MPI ─────────────────────────────────
# $(MPICC) compila y enlaza todos los .c de MPI_SRC en un único ejecutable.
# -Iserver -Inodes -Iimage -Icrypto: rutas de búsqueda de headers (.h)
$(MPI_TARGET): $(MPI_SRC)
	$(MPICC) $(CFLAGS) -Iserver -Inodes -Iimage -Icrypto -o $@ $(MPI_SRC)

# ── Regla de compilación del test de imagen ──────────────────────────────
$(IMAGE_TEST_TARGET): $(IMAGE_SRC)
	$(CC) $(CFLAGS) -Iimage -o $@ $(IMAGE_SRC)

# ── Regla de compilación del test criptográfico ──────────────────────────
$(CRYPTO_TEST_TARGET): $(CRYPTO_SRC)
	$(CC) $(CFLAGS) -Icrypto -o $@ $(CRYPTO_SRC)

# ── Ejecución local con 4 procesos MPI ───────────────────────────────────
# mpirun -np 4: lanzar 4 procesos en la máquina local
# Rank 0 = master, Rank 1-3 = workers (todos en localhost)
run-local: $(MPI_TARGET)
	mpirun -np 4 ./$(MPI_TARGET) test_image.ppm

# ── Ejecutar el test de imagen ────────────────────────────────────────────
test-image: $(IMAGE_TEST_TARGET)
	./$(IMAGE_TEST_TARGET) test_image.ppm

# ── Ejecutar el test criptográfico ───────────────────────────────────────
test-crypto: $(CRYPTO_TEST_TARGET)
	./$(CRYPTO_TEST_TARGET) test_image.ppm

# ── Limpieza de artefactos generados ─────────────────────────────────────
# Elimina binarios y archivos de salida producidos por las pruebas
clean:
	rm -f $(MPI_TARGET)
	rm -f $(IMAGE_TEST_TARGET)
	rm -f $(CRYPTO_TEST_TARGET)
	rm -f output_copy.ppm
	rm -f output_gray.ppm
	rm -f encrypted_output.bin
	rm -f decrypted_output.ppm
	rm -f cluster_output_gray.ppm
	rm -f cluster_decrypted.ppm
	rm -f cluster_encrypted_pixels.bin
