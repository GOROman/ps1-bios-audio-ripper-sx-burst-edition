#include "sx_format.h"

#include <stdint.h>

#define HASH_BITS 12u
#define HASH_SIZE (1u << HASH_BITS)
#define MAX_BLOCK SX_BLOCK_SIZE
#ifndef SX_DEFLATE_MAX_CHAIN
#define SX_DEFLATE_MAX_CHAIN 96u
#endif
#define MAX_CHAIN SX_DEFLATE_MAX_CHAIN
#define MAX_MATCH 258u

static int16_t hash_head[HASH_SIZE];
static int16_t hash_prev[MAX_BLOCK];

static const uint16_t length_base[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const uint8_t length_extra[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const uint16_t distance_base[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
    1025,1537,2049,3073,4097,6145,8193,12289,16385,24577
};
static const uint8_t distance_extra[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

typedef struct {
    uint8_t *dst;
    size_t cap, out;
    uint32_t bits;
    unsigned count;
    int failed;
} bit_writer_t;

typedef struct {
    const uint8_t *src;
    size_t size, in;
    uint32_t bits;
    unsigned count;
    int failed;
} bit_reader_t;

static unsigned reverse_bits(unsigned value, unsigned count) {
    unsigned reversed = 0;
    while (count--) { reversed = (reversed << 1) | (value & 1u); value >>= 1; }
    return reversed;
}

static void put_bits(bit_writer_t *w, unsigned value, unsigned count) {
    if (w->failed) return;
    w->bits |= (uint32_t)value << w->count;
    w->count += count;
    while (w->count >= 8u) {
        if (w->out >= w->cap) { w->failed = 1; return; }
        w->dst[w->out++] = (uint8_t)w->bits;
        w->bits >>= 8; w->count -= 8u;
    }
}

static unsigned get_bits(bit_reader_t *r, unsigned count) {
    while (r->count < count) {
        if (r->in >= r->size) { r->failed = 1; return 0; }
        r->bits |= (uint32_t)r->src[r->in++] << r->count;
        r->count += 8u;
    }
    unsigned value = r->bits & ((1u << count) - 1u);
    r->bits >>= count; r->count -= count;
    return value;
}

static void put_fixed_symbol(bit_writer_t *w, unsigned symbol) {
    unsigned code, count;
    if (symbol <= 143u) { code = 0x30u + symbol; count = 8; }
    else if (symbol <= 255u) { code = 0x190u + symbol - 144u; count = 9; }
    else if (symbol <= 279u) { code = symbol - 256u; count = 7; }
    else { code = 0xc0u + symbol - 280u; count = 8; }
    put_bits(w, reverse_bits(code, count), count);
}

static int get_fixed_symbol(bit_reader_t *r) {
    unsigned code = 0;
    for (unsigned count = 1; count <= 9; count++) {
        code = (code << 1) | get_bits(r, 1);
        if (r->failed) return -1;
        if (count == 7 && code <= 23u) return (int)(256u + code);
        if (count == 8) {
            if (code >= 0x30u && code <= 0xbfu) return (int)(code - 0x30u);
            if (code >= 0xc0u && code <= 0xc7u) return (int)(280u + code - 0xc0u);
        }
        if (count == 9 && code >= 0x190u && code <= 0x1ffu)
            return (int)(144u + code - 0x190u);
    }
    return -1;
}

static unsigned hash3(const uint8_t *p) {
    return ((unsigned)p[0] * 251u + (unsigned)p[1] * 31u + p[2]) & (HASH_SIZE - 1u);
}

static void insert_position(const uint8_t *src, size_t size, size_t position) {
    if (position + 2u >= size) return;
    unsigned hash = hash3(src + position);
    hash_prev[position] = hash_head[hash];
    hash_head[hash] = (int16_t)position;
}

static void find_match(const uint8_t *src, size_t size, size_t position,
                       size_t *best_length, size_t *best_distance) {
    *best_length = 0; *best_distance = 0;
    if (position + 2u >= size) return;
    int candidate = hash_head[hash3(src + position)];
    size_t limit = size - position;
    if (limit > MAX_MATCH) limit = MAX_MATCH;
    for (unsigned chain = 0; candidate >= 0 && chain < MAX_CHAIN; chain++) {
        size_t distance = position - (size_t)candidate;
        if (!distance || distance > 32768u) break;
        size_t length = 0;
        while (length < limit && src[candidate + length] == src[position + length]) length++;
        if (length > *best_length && length >= 3u) {
            *best_length = length; *best_distance = distance;
            if (length == limit) break;
        }
        candidate = hash_prev[candidate];
    }
}

static void put_length_distance(bit_writer_t *w, size_t length, size_t distance) {
    unsigned length_code = 0;
    while (length_code < 28u && length >= length_base[length_code + 1u]) length_code++;
    put_fixed_symbol(w, 257u + length_code);
    unsigned extra = length_extra[length_code];
    if (extra) put_bits(w, (unsigned)(length - length_base[length_code]), extra);

    unsigned distance_code = 0;
    while (distance_code < 29u && distance >= distance_base[distance_code + 1u]) distance_code++;
    put_bits(w, reverse_bits(distance_code, 5), 5);
    extra = distance_extra[distance_code];
    if (extra) put_bits(w, (unsigned)(distance - distance_base[distance_code]), extra);
}

size_t sx_deflate_fixed_encode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap) {
    if (!src || !dst || !size || size > MAX_BLOCK) return 0;
    for (unsigned i = 0; i < HASH_SIZE; i++) hash_head[i] = -1;
    bit_writer_t w = { .dst = dst, .cap = cap };
    put_bits(&w, 1u, 1); /* BFINAL */
    put_bits(&w, 1u, 2); /* fixed Huffman */
    for (size_t position = 0; position < size;) {
        size_t length, distance;
        find_match(src, size, position, &length, &distance);
        if (length >= 3u) {
            put_length_distance(&w, length, distance);
            for (size_t i = 0; i < length; i++) insert_position(src, size, position + i);
            position += length;
        } else {
            put_fixed_symbol(&w, src[position]);
            insert_position(src, size, position++);
        }
    }
    put_fixed_symbol(&w, 256u);
    if (w.count) {
        if (w.out >= w.cap) w.failed = 1;
        else w.dst[w.out++] = (uint8_t)w.bits;
    }
    return w.failed ? 0 : w.out;
}

size_t sx_deflate_fixed_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap) {
    bit_reader_t r = { .src = src, .size = size };
    if (get_bits(&r, 1) != 1u || get_bits(&r, 2) != 1u || r.failed) return 0;
    size_t out = 0;
    for (;;) {
        int symbol = get_fixed_symbol(&r);
        if (symbol < 0) return 0;
        if (symbol < 256) {
            if (out >= cap) return 0;
            dst[out++] = (uint8_t)symbol;
        } else if (symbol == 256) {
            return out;
        } else if (symbol <= 285) {
            unsigned li = (unsigned)symbol - 257u;
            size_t length = length_base[li] + get_bits(&r, length_extra[li]);
            unsigned reversed_distance = get_bits(&r, 5);
            unsigned dc = reverse_bits(reversed_distance, 5);
            if (r.failed || dc >= 30u) return 0;
            size_t distance = distance_base[dc] + get_bits(&r, distance_extra[dc]);
            if (r.failed || distance > out || out + length > cap) return 0;
            while (length--) { dst[out] = dst[out - distance]; out++; }
        } else return 0;
    }
}
