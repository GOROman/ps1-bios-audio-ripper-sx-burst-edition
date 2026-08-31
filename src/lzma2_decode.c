#include <stdlib.h>

#include "sx_format.h"
#include "lzma/Lzma2Dec.h"

typedef struct {
    ISzAlloc vt;
} sx_lzma_allocator_t;

static void *sx_lzma_alloc(ISzAllocPtr allocator, size_t size) {
    (void)allocator;
    return malloc(size);
}

static void sx_lzma_free(ISzAllocPtr allocator, void *address) {
    (void)allocator;
    free(address);
}

static unsigned sx_lzma2_dict_prop(uint8_t dict_log2) {
    /* LZMA2 properties describe a slightly larger dictionary at odd steps.
     * SX V2 deliberately uses exact powers of two, so the even property is
     * the canonical representation (18 -> property 12). */
    if (dict_log2 < 12u || dict_log2 > 27u) return 0xffu;
    return (unsigned)(dict_log2 - 12u) * 2u;
}

size_t sx_lzma2_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                       uint8_t dict_log2) {
    if (!src || !dst || !size || !cap) return 0;
    const unsigned property = sx_lzma2_dict_prop(dict_log2);
    if (property > 40u) return 0;

    sx_lzma_allocator_t allocator = {
        .vt = {sx_lzma_alloc, sx_lzma_free}
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
    if (result != SZ_OK || status != LZMA_STATUS_FINISHED_WITH_MARK ||
        input_size != size) {
        return 0;
    }
    return output_size;
}
