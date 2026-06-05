CC = gcc
MPICC = mpicc

CFLAGS = -Wall -Wextra -Werror -std=c11 -O2

MPI_TARGET = chromascope_mpi
IMAGE_TEST_TARGET = image_test
CRYPTO_TEST_TARGET = crypto_test

MPI_SRC = \
	Server/main_server.c \
	Nodes/worker.c \
	Nodes/resource_monitor.c \
	Image/ppm.c \
	Image/histogram.c \
	Image/rgb_filter.c \
	Crypto/chacha20.c

IMAGE_SRC = \
	Image/ppm.c \
	Image/histogram.c \
	Image/image_test.c \
	Image/rgb_filter.c

CRYPTO_SRC = \
	Crypto/chacha20.c \
	Crypto/crypto_test.c

IMAGE_LIB_SRC = \
	Image/ppm.c \
	Image/histogram.c

CRYPTO_LIB_SRC = \
	Crypto/chacha20.c

IMAGE_OBJ = $(IMAGE_LIB_SRC:.c=.o)
CRYPTO_OBJ = $(CRYPTO_LIB_SRC:.c=.o)

.PHONY: all clean mpi image crypto test-image test-crypto run-local

all: mpi image crypto

mpi: $(MPI_TARGET)

image: $(IMAGE_TEST_TARGET)

crypto: $(CRYPTO_TEST_TARGET)

$(MPI_TARGET): $(MPI_SRC)
	$(MPICC) $(CFLAGS) -Iserver -Inodes -Iimage -Icrypto -o $@ $(MPI_SRC)

$(IMAGE_TEST_TARGET): $(IMAGE_SRC)
	$(CC) $(CFLAGS) -Iimage -o $@ $(IMAGE_SRC)

$(CRYPTO_TEST_TARGET): $(CRYPTO_SRC)
	$(CC) $(CFLAGS) -Icrypto -o $@ $(CRYPTO_SRC)

run-local: $(MPI_TARGET)
	mpirun -np 4 ./$(MPI_TARGET)

test-image: $(IMAGE_TEST_TARGET)
	./$(IMAGE_TEST_TARGET) test_image.ppm

test-crypto: $(CRYPTO_TEST_TARGET)
	./$(CRYPTO_TEST_TARGET)

clean:
	rm -f $(MPI_TARGET)
	rm -f $(IMAGE_TEST_TARGET)
	rm -f $(CRYPTO_TEST_TARGET)
	rm -f *.o
	rm -f image/*.o
	rm -f crypto/*.o
	rm -f server/*.o
	rm -f nodes/*.o
	rm -f output_copy.ppm
	rm -f output_gray.ppm
	rm -f encrypted_output.bin
	rm -f decrypted_output.ppm