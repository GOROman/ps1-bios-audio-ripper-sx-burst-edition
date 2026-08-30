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
    packed[packed_size - 1] ^= 1;
    assert(sx_extract_container(packed, packed_size, output, SX_BIOS_SIZE) == 0);
    printf("PASS raw=%u packed=%u crc32=%08x\n", SX_BIOS_SIZE, (unsigned)packed_size,
           sx_crc32(input, SX_BIOS_SIZE, 0));
    free(output); free(packed); free(input);
    return 0;
}
