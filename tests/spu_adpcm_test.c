#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include "ofdm.h"
#include "ofdm_mod.h"
#include "spu_adpcm.h"
#include "sx_format.h"

#define BLOCKS ((SX_OFDM_PACKET_SAMPLES + SX_SPU_ADPCM_SAMPLES_PER_BLOCK - 1) / SX_SPU_ADPCM_SAMPLES_PER_BLOCK)

int main(void) {
    static uint8_t source[20000], packet[SX_OFDM_PACKET_BYTES];
    static int16_t left[SX_OFDM_PACKET_SAMPLES], right[SX_OFDM_PACKET_SAMPLES];
    static int16_t restored[2][BLOCKS * SX_SPU_ADPCM_SAMPLES_PER_BLOCK];
    static uint8_t encoded[2][BLOCKS][SX_SPU_ADPCM_BLOCK_BYTES];
    sx_spu_adpcm_state_t encoder[2], decoder[2];
    uint64_t signal = 0, noise = 0;

    for (unsigned i = 0; i < sizeof(source); i++) source[i] = (uint8_t)(i * 29u + 7u);
    assert(sx_ofdm_make_packet(source, sizeof(source), sx_crc32(source, sizeof(source), 0), 3, packet));
    sx_ofdm_modulate_packet(packet, 3, left, right);
    for (unsigned channel = 0; channel < 2; channel++) {
        sx_spu_adpcm_reset(&encoder[channel]);
        sx_spu_adpcm_reset(&decoder[channel]);
        for (unsigned block = 0; block < BLOCKS; block++) {
            int16_t padded[SX_SPU_ADPCM_SAMPLES_PER_BLOCK] = { 0 };
            for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
                unsigned index = block * SX_SPU_ADPCM_SAMPLES_PER_BLOCK + i;
                if (index < SX_OFDM_PACKET_SAMPLES) padded[i] = channel ? right[index] : left[index];
            }
            sx_spu_adpcm_encode_block_fast(&encoder[channel], padded, block + 1 == BLOCKS ? 1 : 0,
                                           encoded[channel][block]);
            sx_spu_adpcm_decode_block(&decoder[channel], encoded[channel][block], restored[channel] + block * 28);
        }
    }
    FILE *file = fopen("/tmp/ps1sx-ofdm-adpcm.raw", "wb"); assert(file);
    for (unsigned i = 0; i < SX_OFDM_PACKET_SAMPLES; i++) {
        for (unsigned channel = 0; channel < 2; channel++) {
            int16_t original = channel ? right[i] : left[i], decoded = restored[channel][i];
            int32_t difference = (int32_t)original - decoded;
            signal += (uint64_t)((int64_t)original * original);
            noise += (uint64_t)((int64_t)difference * difference);
            fwrite(&decoded, sizeof(decoded), 1, file);
        }
    }
    fclose(file);
    file = fopen("/tmp/ps1sx-ofdm-adpcm-packet.bin", "wb"); assert(file);
    fwrite(packet, 1, sizeof(packet), file); fclose(file);
    double snr = 10.0 * log10((double)signal / (double)noise);
    assert(snr > 18.0);
    printf("PASS SPU ADPCM blocks=%u stereo_bytes=%u SNR=%.2f dB\n",
           (unsigned)BLOCKS, (unsigned)(BLOCKS * 16 * 2), snr);
    return 0;
}
