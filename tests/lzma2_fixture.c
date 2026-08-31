#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "sx_format.h"

int main(void) {
    enum { INPUT_SIZE = 32768, OUTPUT_CAP = 65536 };
    uint8_t *input = malloc(INPUT_SIZE);
    uint8_t *packed = malloc(OUTPUT_CAP);
    assert(input && packed);
    for (size_t i = 0; i < INPUT_SIZE; i++) {
        input[i] = (uint8_t)((i * 29u + (i >> 7)) & 0xffu);
        if (i >= 12000 && i < 24000) input[i] = (uint8_t)(i & 15u);
    }
    const size_t packed_size = sx_lzma2_encode(input, INPUT_SIZE, packed, OUTPUT_CAP,
                                               3u, 0u, 2u, 18u, NULL, NULL);
    assert(packed_size > 0 && packed_size < INPUT_SIZE);
    FILE *file = fopen("/tmp/ps1sx-lzma2-fixture.bin", "wb");
    assert(file);
    assert(fwrite(packed, 1, packed_size, file) == packed_size);
    assert(fclose(file) == 0);
    printf("PASS LZMA2 fixture %u -> %u bytes\n", INPUT_SIZE, (unsigned)packed_size);

    uint8_t *image = malloc(SX_BIOS_SIZE);
    uint8_t *container = malloc(SX_BIOS_SIZE + 65536u);
    assert(image && container);
    for (size_t i = 0; i < SX_BIOS_SIZE; i++) {
        image[i] = (uint8_t)((i * 29u + (i >> 8)) & 0xffu);
        if (i >= 120000 && i < 360000) image[i] = (uint8_t)(i & 31u);
    }
    const size_t container_size = sx_build_container_progress_mode(
        image, SX_BIOS_SIZE, container, SX_BIOS_SIZE + 65536u,
        NULL, NULL, SX_CONTAINER_V2_LZMA2);
    assert(container_size > sizeof(sx_v2_header_t));
    file = fopen("/tmp/ps1sx-v2-container.bin", "wb");
    assert(file);
    assert(fwrite(container, 1, container_size, file) == container_size);
    assert(fclose(file) == 0);
    printf("PASS V2 container fixture %u -> %u bytes\n", SX_BIOS_SIZE,
           (unsigned)container_size);
    free(container);
    free(image);
    free(packed);
    free(input);
    return 0;
}
