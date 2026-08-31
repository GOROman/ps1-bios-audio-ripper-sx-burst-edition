#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "spu_adpcm.h"

int main(int argc, char **argv) {
    if (argc != 3) return 2;
    FILE *input = fopen(argv[1], "rb");
    FILE *output = fopen(argv[2], "wb");
    if (!input || !output) return 3;
    fseek(input, 0, SEEK_END);
    long bytes = ftell(input);
    rewind(input);
    if (bytes <= 0 || (bytes & 1)) return 4;
    size_t samples_count = (size_t)bytes / 2u;
    int16_t *samples = malloc((size_t)bytes);
    if (!samples || fread(samples, 1, (size_t)bytes, input) != (size_t)bytes) return 5;
    sx_spu_adpcm_state_t state;
    sx_spu_adpcm_reset(&state);
    size_t blocks = (samples_count + SX_SPU_ADPCM_SAMPLES_PER_BLOCK - 1u) /
                    SX_SPU_ADPCM_SAMPLES_PER_BLOCK;
    for (size_t block = 0; block < blocks; block++) {
        int16_t pcm[SX_SPU_ADPCM_SAMPLES_PER_BLOCK] = {0};
        uint8_t encoded[SX_SPU_ADPCM_BLOCK_BYTES];
        for (size_t i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
            size_t at = block * SX_SPU_ADPCM_SAMPLES_PER_BLOCK + i;
            if (at < samples_count) pcm[i] = samples[at];
        }
        sx_spu_adpcm_encode_block(&state, pcm, block + 1u == blocks ? 1u : 0u, encoded);
        if (fwrite(encoded, 1, sizeof(encoded), output) != sizeof(encoded)) return 6;
    }
    fclose(output);
    fclose(input);
    free(samples);
    fprintf(stderr, "%zu samples -> %zu ADPCM bytes\n", samples_count,
            blocks * SX_SPU_ADPCM_BLOCK_BYTES);
    return 0;
}
