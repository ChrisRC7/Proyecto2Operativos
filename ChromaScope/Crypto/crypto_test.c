#include "chacha20.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

static int test_text_encryption(void);
static int test_file_encryption(const char *input_path);
static int read_entire_file(const char *path, uint8_t **buffer, size_t *size);
static int write_entire_file(const char *path, const uint8_t *buffer, size_t size);
static void fill_test_key_nonce(uint8_t key[CHACHA20_KEY_SIZE], uint8_t nonce[CHACHA20_NONCE_SIZE]);

int main(int argc, char **argv) {
    if (test_text_encryption() != 0) {
        fprintf(stderr, "Error: fallo la prueba de texto.\n");
        return EXIT_FAILURE;
    }

    printf("Prueba de texto completada correctamente.\n");

    if (argc == 2) {
        if (test_file_encryption(argv[1]) != 0) {
            fprintf(stderr, "Error: fallo la prueba con archivo.\n");
            return EXIT_FAILURE;
        }

        printf("Prueba de archivo completada correctamente.\n");
    } else {
        printf("No se proporciono archivo. Se omitio prueba con archivo.\n");
        printf("Uso opcional:\n");
        printf("  ./crypto_test imagen.ppm\n");
    }

    return EXIT_SUCCESS;
}

static int test_text_encryption(void) {
    const char *message = "ChromaScope MPI ChaCha20 test message";

    size_t length = strlen(message) + 1;

    uint8_t *encrypted = malloc(length);
    uint8_t *decrypted = malloc(length);

    if (encrypted == NULL || decrypted == NULL) {
        free(encrypted);
        free(decrypted);
        return -1;
    }

    uint8_t key[CHACHA20_KEY_SIZE];
    uint8_t nonce[CHACHA20_NONCE_SIZE];

    fill_test_key_nonce(key, nonce);

    chacha20_encrypt_buffer(
        key,
        nonce,
        1,
        (const uint8_t *)message,
        encrypted,
        length
    );

    chacha20_encrypt_buffer(
        key,
        nonce,
        1,
        encrypted,
        decrypted,
        length
    );

    int result = memcmp(message, decrypted, length);

    free(encrypted);
    free(decrypted);

    return result == 0 ? 0 : -1;
}

static int test_file_encryption(const char *input_path) {
    uint8_t *original = NULL;
    size_t original_size = 0;

    if (read_entire_file(input_path, &original, &original_size) != 0) {
        return -1;
    }

    uint8_t *encrypted = malloc(original_size);
    uint8_t *decrypted = malloc(original_size);

    if (encrypted == NULL || decrypted == NULL) {
        free(original);
        free(encrypted);
        free(decrypted);
        return -1;
    }

    uint8_t key[CHACHA20_KEY_SIZE];
    uint8_t nonce[CHACHA20_NONCE_SIZE];

    fill_test_key_nonce(key, nonce);

    chacha20_encrypt_buffer(
        key,
        nonce,
        1,
        original,
        encrypted,
        original_size
    );

    chacha20_encrypt_buffer(
        key,
        nonce,
        1,
        encrypted,
        decrypted,
        original_size
    );

    if (write_entire_file("encrypted_output.bin", encrypted, original_size) != 0) {
        free(original);
        free(encrypted);
        free(decrypted);
        return -1;
    }

    if (write_entire_file("decrypted_output.ppm", decrypted, original_size) != 0) {
        free(original);
        free(encrypted);
        free(decrypted);
        return -1;
    }

    int result = memcmp(original, decrypted, original_size);

    printf("Archivo original: %s\n", input_path);
    printf("Archivo cifrado: encrypted_output.bin\n");
    printf("Archivo descifrado: decrypted_output.ppm\n");
    printf("Bytes procesados: %zu\n", original_size);

    free(original);
    free(encrypted);
    free(decrypted);

    return result == 0 ? 0 : -1;
}

static int read_entire_file(const char *path, uint8_t **buffer, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        fprintf(stderr, "Error: no se pudo abrir archivo: %s\n", path);
        return -1;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return -1;
    }

    long file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return -1;
    }

    rewind(file);

    uint8_t *data = malloc((size_t)file_size);
    if (data == NULL) {
        fclose(file);
        return -1;
    }

    size_t bytes_read = fread(data, 1, (size_t)file_size, file);
    fclose(file);

    if (bytes_read != (size_t)file_size) {
        free(data);
        return -1;
    }

    *buffer = data;
    *size = (size_t)file_size;

    return 0;
}

static int write_entire_file(const char *path, const uint8_t *buffer, size_t size) {
    FILE *file = fopen(path, "wb");
    if (file == NULL) {
        fprintf(stderr, "Error: no se pudo crear archivo: %s\n", path);
        return -1;
    }

    size_t bytes_written = fwrite(buffer, 1, size, file);
    fclose(file);

    return bytes_written == size ? 0 : -1;
}

static void fill_test_key_nonce(uint8_t key[CHACHA20_KEY_SIZE], uint8_t nonce[CHACHA20_NONCE_SIZE]) {
    for (int i = 0; i < CHACHA20_KEY_SIZE; i++) {
        key[i] = (uint8_t)i;
    }

    for (int i = 0; i < CHACHA20_NONCE_SIZE; i++) {
        nonce[i] = (uint8_t)(i + 100);
    }
}