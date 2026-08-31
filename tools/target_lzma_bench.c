#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "sx_format.h"

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 3;
    uint8_t *input = malloc(SX_BIOS_SIZE);
    uint8_t *output = malloc(SX_BIOS_SIZE + 65536u);
    if (!input || !output) return 4;
    if (fread(input, 1, SX_BIOS_SIZE, file) != SX_BIOS_SIZE || fgetc(file) != EOF) return 5;
    fclose(file);
    size_t packed = sx_build_container_progress_mode(input, SX_BIOS_SIZE, output,
                                                      SX_BIOS_SIZE + 65536u,
                                                      NULL, NULL,
                                                      SX_CONTAINER_V2_LZMA2);
    if (!packed) {
        const sx_container_diagnostics_t *d = sx_container_diagnostics();
        fprintf(stderr, "ERROR E%d request=%zu\n", d->lzma_last_error,
                d->lzma_last_request);
        return 6;
    }
    uint8_t *restored = malloc(SX_BIOS_SIZE);
    if (!restored || sx_extract_container(output, packed, restored, SX_BIOS_SIZE) != SX_BIOS_SIZE)
        return 7;
    for (size_t i = 0; i < SX_BIOS_SIZE; i++) if (restored[i] != input[i]) return 8;
    printf("%zu\n", packed);
    free(restored); free(output); free(input);
    return 0;
}
