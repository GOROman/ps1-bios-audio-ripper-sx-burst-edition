#pragma once
#include <stddef.h>
#include <stdint.h>

#define SX_SPU_ADPCM_SAMPLES_PER_BLOCK 28u
#define SX_SPU_ADPCM_BLOCK_BYTES       16u

typedef struct {
    int32_t previous_1;
    int32_t previous_2;
} sx_spu_adpcm_state_t;

void sx_spu_adpcm_reset(sx_spu_adpcm_state_t *state);
void sx_spu_adpcm_encode_block(sx_spu_adpcm_state_t *state,
                               const int16_t pcm[SX_SPU_ADPCM_SAMPLES_PER_BLOCK],
                               uint8_t flags,
                               uint8_t output[SX_SPU_ADPCM_BLOCK_BYTES]);
void sx_spu_adpcm_encode_block_fast(sx_spu_adpcm_state_t *state,
                                    const int16_t pcm[SX_SPU_ADPCM_SAMPLES_PER_BLOCK],
                                    uint8_t flags,
                                    uint8_t output[SX_SPU_ADPCM_BLOCK_BYTES]);
void sx_spu_adpcm_decode_block(sx_spu_adpcm_state_t *state,
                               const uint8_t input[SX_SPU_ADPCM_BLOCK_BYTES],
                               int16_t pcm[SX_SPU_ADPCM_SAMPLES_PER_BLOCK]);
