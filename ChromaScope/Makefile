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
	Crypto/chacha20.c

IMAGE_SRC = \
	Image/ppm.c \
	Image/histogram.c \
	Image/image_test.c

CRYPTO_SRC = \
	Crypto/chacha20.c \
	Crypto/crypto_test.c

.PHONY: all clean mpi image crypto run-local test-image test-crypto

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
	mpirun -np 4 ./$(MPI_TARGET) test_image.ppm

test-image: $(IMAGE_TEST_TARGET)
	./$(IMAGE_TEST_TARGET) test_image.ppm

test-crypto: $(CRYPTO_TEST_TARGET)
	./$(CRYPTO_TEST_TARGET) test_image.ppm

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