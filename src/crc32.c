#include "sx_format.h"

uint32_t sx_crc32(const void *data, size_t size, uint32_t seed) {
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~seed;
    while (size--) {
        crc ^= *p++;
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1));
    }
    return ~crc;
}
