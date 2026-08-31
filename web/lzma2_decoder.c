#include <stddef.h>
#include <stdint.h>

#include "sx_format.h"
#include "lzma/Lzma2Dec.h"

/* The browser keeps the compressed area and restored area in these exported
 * buffers. Decoder probabilities and the 256 KiB dictionary use a separate
 * bump arena so no JavaScript-side allocator or libc is required. */
#define SX_LZMA2_BUFFER_SIZE (SX_BIOS_SIZE)
#define SX_LZMA2_HEAP_SIZE (1u << 20)

uint8_t sx_lzma2_input[SX_LZMA2_BUFFER_SIZE];
uint8_t sx_lzma2_output[SX_LZMA2_BUFFER_SIZE];
static uint8_t sx_lzma2_heap[SX_LZMA2_HEAP_SIZE];
static size_t sx_lzma2_heap_pos;

typedef struct {
    ISzAlloc vt;
} sx_lzma_allocator_t;

void *memcpy(void *destination, const void *source, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    const uint8_t *in = (const uint8_t *)source;
    for (size_t i = 0; i < size; i++) out[i] = in[i];
    return destination;
}

void *memset(void *destination, int value, size_t size) {
    uint8_t *out = (uint8_t *)destination;
    for (size_t i = 0; i < size; i++) out[i] = (uint8_t)value;
    return destination;
}

static void *sx_lzma2_alloc(ISzAllocPtr allocator, size_t size) {
    (void)allocator;
    size = (size + 7u) & ~7u;
    if (!size || size > sizeof(sx_lzma2_heap) - sx_lzma2_heap_pos) return NULL;
    void *result = sx_lzma2_heap + sx_lzma2_heap_pos;
    sx_lzma2_heap_pos += size;
    return result;
}

static void sx_lzma2_free(ISzAllocPtr allocator, void *address) {
    (void)allocator;
    (void)address;
}

uint32_t sx_lzma2_input_ptr(void) {
    return (uint32_t)(uintptr_t)sx_lzma2_input;
}

uint32_t sx_lzma2_output_ptr(void) {
    return (uint32_t)(uintptr_t)sx_lzma2_output;
}

static unsigned sx_lzma2_dict_prop(uint8_t dict_log2) {
    if (dict_log2 < 12u || dict_log2 > 27u) return 0xffu;
    return (unsigned)(dict_log2 - 12u) * 2u;
}

size_t sx_lzma2_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                       uint8_t dict_log2) {
    if (!src || !dst || !size || !cap) return 0;
    const unsigned property = sx_lzma2_dict_prop(dict_log2);
    if (property > 40u) return 0;
    sx_lzma2_heap_pos = 0;
    sx_lzma_allocator_t allocator = {
        .vt = {sx_lzma2_alloc, sx_lzma2_free}
    };
    CLzma2Dec decoder;
    Lzma2Dec_Construct(&decoder);
    if (Lzma2Dec_Allocate(&decoder, (Byte)property, &allocator.vt) != SZ_OK) return 0;
    Lzma2Dec_Init(&decoder);
    size_t input_size = size;
    size_t output_size = cap;
    ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;
    const SRes result = Lzma2Dec_DecodeToBuf(&decoder, dst, &output_size, src, &input_size,
                                             LZMA_FINISH_END, &status);
    Lzma2Dec_Free(&decoder, &allocator.vt);
    return result == SZ_OK && status == LZMA_STATUS_FINISHED_WITH_MARK &&
           input_size == size ? output_size : 0;
}
