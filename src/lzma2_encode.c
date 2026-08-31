#include <stdlib.h>

#include "sx_format.h"
#include "lzma/Lzma2Enc.h"

typedef struct {
    ISzAlloc vt;
#ifdef SX_LZMA_TAIL_ARENA
    uintptr_t floor;
    uintptr_t cursor;
#endif
} sx_lzma_allocator_t;

static int sx_lzma_error;
static size_t sx_lzma_request;

static void *sx_lzma_alloc(ISzAllocPtr allocator, size_t size) {
    if (size > sx_lzma_request) sx_lzma_request = size;
#ifdef SX_LZMA_TAIL_ARENA
    sx_lzma_allocator_t *arena = (sx_lzma_allocator_t *)allocator;
    if (!size || size > arena->cursor - arena->floor) return NULL;
    uintptr_t next = (arena->cursor - size) & ~(uintptr_t)15u;
    if (next < arena->floor) return NULL;
    arena->cursor = next;
    return (void *)next;
#else
    (void)allocator;
    return malloc(size);
#endif
}

static void sx_lzma_free(ISzAllocPtr allocator, void *address) {
#ifdef SX_LZMA_TAIL_ARENA
    /* Every encode gets a fresh arena.  The 7-Zip encoder keeps its working
     * allocations until Destroy(), so individual frees need not recycle. */
    (void)allocator;
    (void)address;
#else
    (void)allocator;
    free(address);
#endif
}

#ifdef SX_LZMA_TAIL_ARENA
#define SX_LZMA_STACK_GUARD (64u * 1024u)

static int sx_lzma_init_tail_arena(sx_lzma_allocator_t *arena) {
    uintptr_t stack;
    __asm__ volatile("move %0, $sp" : "=r"(stack));
    uintptr_t floor = ((uintptr_t)sbrk(0) + 15u) & ~(uintptr_t)15u;
    if (stack <= SX_LZMA_STACK_GUARD) return 0;
    uintptr_t top = (stack - SX_LZMA_STACK_GUARD) & ~(uintptr_t)15u;
    if (top <= floor) return 0;
    arena->floor = floor;
    arena->cursor = top;
    return 1;
}
#endif

typedef struct {
    ICompressProgress vt;
    sx_lzma_progress_fn callback;
    void *user;
    size_t total;
} sx_lzma_progress_t;

static SRes sx_lzma_progress(ICompressProgressPtr progress, UInt64 completed, UInt64 output) {
    sx_lzma_progress_t *state = Z7_CONTAINER_FROM_VTBL(progress, sx_lzma_progress_t, vt);
    (void)output;
    if (state->callback) {
        size_t value = completed > (UInt64)state->total ? state->total : (size_t)completed;
        state->callback(value, state->total, state->user);
    }
    return SZ_OK;
}

size_t sx_lzma2_encode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                       uint8_t lc, uint8_t lp, uint8_t pb, uint8_t dict_log2,
                       sx_lzma_progress_fn progress, void *user) {
    sx_lzma_error = SZ_OK;
    sx_lzma_request = 0;
    if (!src || !dst || !size || !cap || lc > 8u || lp > 4u || pb > 4u ||
        lc + lp > 4u || dict_log2 < 12u || dict_log2 > 27u) {
        sx_lzma_error = SZ_ERROR_PARAM;
        return 0;
    }

    sx_lzma_allocator_t allocator = {
        .vt = {sx_lzma_alloc, sx_lzma_free}
    };
#ifdef SX_LZMA_TAIL_ARENA
    if (!sx_lzma_init_tail_arena(&allocator)) {
        sx_lzma_error = SZ_ERROR_MEM;
        return 0;
    }
#endif
    CLzma2EncHandle encoder = Lzma2Enc_Create(&allocator.vt, &allocator.vt);
    if (!encoder) {
        sx_lzma_error = SZ_ERROR_MEM;
        return 0;
    }

    CLzma2EncProps props;
    Lzma2EncProps_Init(&props);
    props.blockSize = LZMA2_ENC_PROPS_BLOCK_SIZE_SOLID;
    /* The PS1 build opts into a small match finder.  A 256 KiB dictionary
     * plus the normal binary-tree hash tables cannot coexist with the
     * resident container and modem buffers in 2 MiB of main RAM.  The
     * stream remains standard LZMA2; only the encoder search profile changes.
     */
#ifdef SX_LZMA_LOW_MEMORY
    props.lzmaProps.level = 3;
#else
    props.lzmaProps.level = 9;
#endif
    props.lzmaProps.dictSize = (UInt32)1u << dict_log2;
    props.lzmaProps.lc = lc;
    props.lzmaProps.lp = lp;
    props.lzmaProps.pb = pb;
    props.lzmaProps.algo = 1;
    props.lzmaProps.fb = 273;
    props.lzmaProps.btMode = 1;
    props.lzmaProps.numHashBytes = 4;
    props.lzmaProps.numHashOutBits = 0;
    props.lzmaProps.numThreads = 1;
    props.lzmaProps.writeEndMark = 0;
#ifdef SX_LZMA_LOW_MEMORY
    props.lzmaProps.algo = 0;
    props.lzmaProps.fb = 32;
    props.lzmaProps.numHashBytes = 2;
    props.lzmaProps.numHashOutBits = 16;
    props.lzmaProps.mc = 16;
#endif

    SRes result = Lzma2Enc_SetProps(encoder, &props);
    if (result == SZ_OK) {
        Lzma2Enc_SetDataSize(encoder, size);
        sx_lzma_progress_t progress_state = {
            .vt = {sx_lzma_progress},
            .callback = progress,
            .user = user,
            .total = size
        };
        size_t output_size = cap;
        result = Lzma2Enc_Encode2(encoder, NULL, dst, &output_size, NULL, src, size,
                                  progress ? &progress_state.vt : NULL);
        if (result == SZ_OK) {
            Lzma2Enc_Destroy(encoder);
            return output_size;
        }
    }
    sx_lzma_error = result;
    Lzma2Enc_Destroy(encoder);
    return 0;
}

int sx_lzma2_last_error(void) { return sx_lzma_error; }
size_t sx_lzma2_last_request(void) { return sx_lzma_request; }
