#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "sx_format.h"

static size_t best_area(const uint8_t *src, size_t size, uint8_t *encoded,
                        uint8_t *decoded, unsigned *best_lc, unsigned *best_lp,
                        unsigned *best_pb) {
    size_t best = SIZE_MAX;
    for (unsigned lc = 0; lc <= 4; lc++) {
        for (unsigned lp = 0; lp <= 4 - lc; lp++) {
            for (unsigned pb = 0; pb <= 4; pb++) {
                size_t packed = sx_lzma2_encode(src, size, encoded, size + 65536u,
                                                (uint8_t)lc, (uint8_t)lp, (uint8_t)pb,
                                                SX_LZMA2_DICT_LOG2, NULL, NULL);
                if (!packed || packed >= best) continue;
                if (sx_lzma2_decode(encoded, packed, decoded, size,
                                    SX_LZMA2_DICT_LOG2) != size) return SIZE_MAX;
                for (size_t i = 0; i < size; i++) if (decoded[i] != src[i]) return SIZE_MAX;
                best = packed; *best_lc = lc; *best_lp = lp; *best_pb = pb;
            }
        }
    }
    return best;
}

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    unsigned long split_value = strtoul(argv[2], NULL, 0);
    if (split_value < 0x20000u || split_value > 0x70000u || (split_value & 3u)) return 3;
    FILE *file = fopen(argv[1], "rb");
    if (!file) return 4;
    uint8_t *input = malloc(SX_BIOS_SIZE);
    uint8_t *encoded = malloc(SX_BIOS_SIZE + 65536u);
    uint8_t *decoded = malloc(SX_BIOS_SIZE);
    if (!input || !encoded || !decoded) return 5;
    if (fread(input, 1, SX_BIOS_SIZE, file) != SX_BIOS_SIZE || fgetc(file) != EOF) return 6;
    fclose(file);
    size_t split = (size_t)split_value;
    unsigned lc0=0,lp0=0,pb0=0,lc1=0,lp1=0,pb1=0;
    size_t first = best_area(input, split, encoded, decoded, &lc0, &lp0, &pb0);
    size_t second = best_area(input + split, SX_BIOS_SIZE - split, encoded, decoded,
                              &lc1, &lp1, &pb1);
    if (first == SIZE_MAX || second == SIZE_MAX) return 7;
    printf("%zu split=0x%06zx a0=%u/%u/%u:%zu a1=%u/%u/%u:%zu\n",
           32u + 56u + first + second, split,
           lc0,lp0,pb0,first,lc1,lp1,pb1,second);
    return 0;
}
