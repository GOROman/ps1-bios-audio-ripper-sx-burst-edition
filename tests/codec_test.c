#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sx_format.h"

#define CAP (SX_BIOS_SIZE + 65536u)

int main(void) {
    uint8_t *input = malloc(SX_BIOS_SIZE), *packed = malloc(CAP), *output = malloc(SX_BIOS_SIZE);
    assert(input && packed && output);
    for (size_t i = 0; i < SX_BIOS_SIZE; i++) input[i] = (uint8_t)((i >> 3) ^ (i * 17u));
    memset(input + 32768, 0, 32768);
    size_t packed_size = sx_build_container(input, SX_BIOS_SIZE, packed, CAP);
    assert(packed_size > sizeof(sx_header_t));
    assert(sx_extract_container(packed, packed_size, output, SX_BIOS_SIZE) == SX_BIOS_SIZE);
    assert(!memcmp(input, output, SX_BIOS_SIZE));

    uint8_t deflated[SX_BLOCK_SIZE + SX_BLOCK_SIZE / 8u + 16u];
    size_t deflated_size = sx_deflate_fixed_encode(input, SX_BLOCK_SIZE, deflated, sizeof(deflated));
    assert(deflated_size > 0);
    memset(output, 0, SX_BLOCK_SIZE);
    assert(sx_deflate_fixed_decode(deflated, deflated_size, output, SX_BLOCK_SIZE) == SX_BLOCK_SIZE);
    assert(!memcmp(input, output, SX_BLOCK_SIZE));
    packed[packed_size - 1] ^= 1;
    assert(sx_extract_container(packed, packed_size, output, SX_BIOS_SIZE) == 0);
    printf("PASS raw=%u packed=%u fixed=%u crc32=%08x\n", SX_BIOS_SIZE, (unsigned)packed_size,
           (unsigned)deflated_size,
           sx_crc32(input, SX_BIOS_SIZE, 0));
    free(output); free(packed); free(input);
    return 0;
}
