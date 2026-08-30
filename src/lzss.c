#include "sx_format.h"

#define HASH_SIZE 4096
#define WINDOW_SIZE 4096
#define MATCH_MAX 18

static unsigned hash3(const uint8_t *p) {
    return ((unsigned)p[0] * 251u + (unsigned)p[1] * 17u + p[2]) & (HASH_SIZE - 1);
}

size_t sx_lzss_encode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap) {
    int last[HASH_SIZE];
    for (int i = 0; i < HASH_SIZE; i++) last[i] = -1;
    size_t in = 0, out = 0;
    while (in < size) {
        if (out >= cap) return 0;
        size_t flag_pos = out++;
        uint8_t flags = 0;
        for (unsigned bit = 0; bit < 8 && in < size; bit++) {
            int candidate = -1;
            if (in + 2 < size) {
                unsigned hash = hash3(src + in);
                candidate = last[hash];
                last[hash] = (int)in;
            }
            size_t match = 0;
            if (candidate >= 0 && in - (size_t)candidate <= WINDOW_SIZE) {
                size_t limit = size - in;
                if (limit > MATCH_MAX) limit = MATCH_MAX;
                while (match < limit && src[candidate + match] == src[in + match]) match++;
            }
            if (match >= 3) {
                if (out + 2 > cap) return 0;
                size_t distance = in - (size_t)candidate - 1;
                dst[out++] = (uint8_t)distance;
                dst[out++] = (uint8_t)((distance >> 8) | ((match - 3) << 4));
                for (size_t i = 1; i < match; i++)
                    if (in + i + 2 < size) last[hash3(src + in + i)] = (int)(in + i);
                in += match;
            } else {
                if (out >= cap) return 0;
                flags |= (uint8_t)(1u << bit);
                dst[out++] = src[in++];
            }
        }
        dst[flag_pos] = flags;
    }
    return out;
}

size_t sx_lzss_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap) {
    size_t in = 0, out = 0;
    while (in < size) {
        uint8_t flags = src[in++];
        for (unsigned bit = 0; bit < 8 && in < size; bit++) {
            if (flags & (1u << bit)) {
                if (out >= cap) return 0;
                dst[out++] = src[in++];
            } else {
                if (in + 2 > size) return 0;
                unsigned lo = src[in++], hi = src[in++];
                size_t distance = 1u + lo + ((hi & 15u) << 8);
                size_t length = 3u + (hi >> 4);
                if (distance > out || out + length > cap) return 0;
                for (size_t i = 0; i < length; i++) {
                    dst[out] = dst[out - distance];
                    out++;
                }
            }
        }
    }
    return out;
}
