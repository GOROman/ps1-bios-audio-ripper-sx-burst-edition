#include <limits.h>
#include <stdint.h>
#include "spu_adpcm.h"

static const int16_t coefficients[5][2] = {
    { 0, 0 }, { 60, 0 }, { 115, -52 }, { 98, -55 }, { 122, -60 }
};

static int32_t clamp16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return value;
}

static int32_t predict(const sx_spu_adpcm_state_t *state, unsigned filter) {
    return (state->previous_1 * coefficients[filter][0]
          + state->previous_2 * coefficients[filter][1] + 32) >> 6;
}

static int quantize(int32_t residual, unsigned shift) {
    unsigned bits = 12u - shift;
    int32_t half = bits ? 1 << (bits - 1u) : 0;
    int32_t value = residual >= 0 ? (residual + half) >> bits
                                  : -(((-residual) + half) >> bits);
    if (value < -8) return -8;
    if (value > 7) return 7;
    return (int)value;
}

static int32_t reconstruct(sx_spu_adpcm_state_t *state, unsigned filter,
                           unsigned shift, int nibble);

void sx_spu_adpcm_encode_block_fast(sx_spu_adpcm_state_t *state,
                                    const int16_t pcm[SX_SPU_ADPCM_SAMPLES_PER_BLOCK],
                                    uint8_t flags,
                                    uint8_t output[SX_SPU_ADPCM_BLOCK_BYTES]) {
    uint32_t positive = 0, negative = 0;
    for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
        if (pcm[i] >= 0) {
            if ((uint32_t)pcm[i] > positive) positive = (uint32_t)pcm[i];
        } else {
            uint32_t magnitude = (uint32_t)-(int32_t)pcm[i];
            if (magnitude > negative) negative = magnitude;
        }
    }
    unsigned shift = 12;
    while (shift && (positive > (7u << (12u - shift)) || negative > (8u << (12u - shift)))) shift--;
    output[0] = (uint8_t)shift; /* filter 0 */
    output[1] = flags;
    for (unsigned i = 0; i < 14; i++) output[i + 2] = 0;
    for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
        int nibble = quantize(pcm[i], shift);
        reconstruct(state, 0, shift, nibble);
        output[2 + i / 2] |= (uint8_t)((nibble & 15) << ((i & 1) * 4));
    }
}

static int32_t reconstruct(sx_spu_adpcm_state_t *state, unsigned filter,
                           unsigned shift, int nibble) {
    int32_t sample = clamp16(predict(state, filter) + (nibble << (12u - shift)));
    state->previous_2 = state->previous_1;
    state->previous_1 = sample;
    return sample;
}

void sx_spu_adpcm_reset(sx_spu_adpcm_state_t *state) {
    state->previous_1 = 0;
    state->previous_2 = 0;
}

void sx_spu_adpcm_encode_block(sx_spu_adpcm_state_t *state,
                               const int16_t pcm[SX_SPU_ADPCM_SAMPLES_PER_BLOCK],
                               uint8_t flags,
                               uint8_t output[SX_SPU_ADPCM_BLOCK_BYTES]) {
    uint64_t best_error = UINT64_MAX;
    unsigned best_filter = 0, best_shift = 0;

    for (unsigned filter = 0; filter < 5; filter++) {
        /* Estimate the useful quantizer range from the unquantized predictor,
         * then evaluate its immediate neighbors with the real feedback loop.
         * This cuts 65 trials to at most 15 without blindly lowering quality. */
        sx_spu_adpcm_state_t analysis = *state;
        uint32_t peak_residual = 0;
        for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
            int32_t residual = (int32_t)pcm[i] - predict(&analysis, filter);
            uint32_t magnitude = residual < 0 ? (uint32_t)(-residual) : (uint32_t)residual;
            if (magnitude > peak_residual) peak_residual = magnitude;
            analysis.previous_2 = analysis.previous_1;
            analysis.previous_1 = pcm[i];
        }
        unsigned center = 12;
        while (center && peak_residual > (7u << (12u - center))) center--;
        unsigned first = center ? center - 1u : 0u;
        unsigned last = center < 12 ? center + 1u : 12u;
        for (unsigned shift = first; shift <= last; shift++) {
            sx_spu_adpcm_state_t trial = *state;
            uint64_t error = 0;
            for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
                int nibble = quantize((int32_t)pcm[i] - predict(&trial, filter), shift);
                int32_t decoded = reconstruct(&trial, filter, shift, nibble);
                int32_t difference = (int32_t)pcm[i] - decoded;
                error += (uint64_t)((int64_t)difference * difference);
            }
            if (error < best_error) {
                best_error = error;
                best_filter = filter;
                best_shift = shift;
            }
        }
    }

    output[0] = (uint8_t)((best_filter << 4) | best_shift);
    output[1] = flags;
    for (unsigned i = 0; i < 14; i++) output[i + 2] = 0;
    for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
        int nibble = quantize((int32_t)pcm[i] - predict(state, best_filter), best_shift);
        reconstruct(state, best_filter, best_shift, nibble);
        output[2 + i / 2] |= (uint8_t)((nibble & 15) << ((i & 1) * 4));
    }
}

void sx_spu_adpcm_decode_block(sx_spu_adpcm_state_t *state,
                               const uint8_t input[SX_SPU_ADPCM_BLOCK_BYTES],
                               int16_t pcm[SX_SPU_ADPCM_SAMPLES_PER_BLOCK]) {
    unsigned shift = input[0] & 15, filter = input[0] >> 4;
    if (shift > 12) shift = 12;
    if (filter > 4) filter = 0;
    for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
        int nibble = (input[2 + i / 2] >> ((i & 1) * 4)) & 15;
        if (nibble & 8) nibble -= 16;
        pcm[i] = (int16_t)reconstruct(state, filter, shift, nibble);
    }
}
