/* End-to-end transmit-side generator for the local loopback test.
 *
 * Mirrors the real PS1 path in src/ofdm_tx.c: build a container, then for every
 * OFDM packet run sx_ofdm_modulate_packet() followed by an SPU-ADPCM encode and
 * decode (state reset per packet per channel, ADPCM_BLOCKS blocks each).  The
 * decoded 16-bit stereo stream is what a perfect capture of the console output
 * would contain; tests/ofdm_loopback_test.js adds channel impairments on top.
 *
 *   ./gen [payload_bytes] [stream.wav] [container.bin]
 *
 * The stream is written as a 16-bit stereo PCM WAV at 44.1 kHz -- the same thing
 * a clean line-in recording of the console would produce -- so it can be played
 * or inspected directly.  stdout carries the parameters the JS side needs
 * (KEY=VALUE lines).
 */
#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ofdm.h"
#include "ofdm_mod.h"
#include "spu_adpcm.h"
#include "sx_format.h"

#define ADPCM_BLOCKS ((SX_OFDM_PACKET_SAMPLES + SX_SPU_ADPCM_SAMPLES_PER_BLOCK - 1u) / SX_SPU_ADPCM_SAMPLES_PER_BLOCK)
#define CHANNEL_SAMPLES (ADPCM_BLOCKS * SX_SPU_ADPCM_SAMPLES_PER_BLOCK)

static uint8_t source[SX_BIOS_SIZE];
static uint8_t container[SX_BIOS_SIZE + 65536u];
static uint8_t packet[SX_OFDM_PACKET_BYTES];
static int16_t left[SX_OFDM_PACKET_SAMPLES], right[SX_OFDM_PACKET_SAMPLES];
static int16_t restored[2][CHANNEL_SAMPLES];
static uint8_t encoded[ADPCM_BLOCKS][SX_SPU_ADPCM_BLOCK_BYTES];

/* Canonical 44-byte RIFF/WAVE header for 16-bit PCM.  The host is little-endian
 * (x86 / Apple silicon), so the multi-byte integer writes already match WAV. */
static void write_wav_header(FILE *file, uint32_t data_bytes, uint32_t rate,
                             uint16_t channels, uint16_t bits) {
    uint32_t byte_rate = rate * channels * (bits / 8u);
    uint16_t block_align = (uint16_t)(channels * (bits / 8u));
    uint32_t riff_size = 36u + data_bytes;
    uint32_t fmt_size = 16u;
    uint16_t pcm_format = 1u;
    fwrite("RIFF", 1, 4, file); fwrite(&riff_size, 4, 1, file); fwrite("WAVE", 1, 4, file);
    fwrite("fmt ", 1, 4, file); fwrite(&fmt_size, 4, 1, file); fwrite(&pcm_format, 2, 1, file);
    fwrite(&channels, 2, 1, file); fwrite(&rate, 4, 1, file); fwrite(&byte_rate, 4, 1, file);
    fwrite(&block_align, 2, 1, file); fwrite(&bits, 2, 1, file);
    fwrite("data", 1, 4, file); fwrite(&data_bytes, 4, 1, file);
}

int main(int argc, char **argv) {
    size_t payload_bytes = argc > 1 ? (size_t)strtoul(argv[1], NULL, 0) : SX_BLOCK_SIZE;
    const char *stream_path = argc > 2 ? argv[2] : "/tmp/ps1sx-loopback.wav";
    const char *container_path = argc > 3 ? argv[3] : "/tmp/ps1sx-loopback-container.bin";
    if (payload_bytes == 0 || payload_bytes > sizeof(source)) {
        fprintf(stderr, "payload_bytes out of range\n");
        return 1;
    }

    /* Incompressible generated data so the container keeps RAW blocks and the
     * packet count stays representative (no BIOS-like content). */
    uint32_t rng = 0x1234abcdu;
    for (size_t i = 0; i < payload_bytes; i++) {
        rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
        source[i] = (uint8_t)(rng >> 24);
    }

    size_t packed = sx_build_container(source, payload_bytes, container, sizeof(container));
    assert(packed >= sizeof(sx_header_t));
    uint32_t container_crc = sx_crc32(container, packed, 0);

    FILE *cf = fopen(container_path, "wb");
    assert(cf);
    assert(fwrite(container, 1, packed, cf) == packed);
    fclose(cf);

    size_t packet_count = sx_ofdm_packet_count(packed);
    FILE *sf = fopen(stream_path, "wb");
    assert(sf);
    uint32_t data_bytes = (uint32_t)(packet_count * CHANNEL_SAMPLES * 2u * sizeof(int16_t));
    write_wav_header(sf, data_bytes, 44100u, 2u, 16u);

    uint64_t signal = 0, noise = 0;
    for (size_t index = 0; index < packet_count; index++) {
        int ok = sx_ofdm_make_packet(container, packed, container_crc, index, packet);
        assert(ok);
        sx_ofdm_modulate_packet(packet, (uint32_t)index, left, right);
        uint8_t final_flags = (index + 1u == packet_count) ? 1u : 0u;
        for (unsigned channel = 0; channel < 2; channel++) {
            const int16_t *pcm = channel ? right : left;
            sx_spu_adpcm_state_t enc, dec;
            sx_spu_adpcm_reset(&enc);
            sx_spu_adpcm_reset(&dec);
            for (unsigned block = 0; block < ADPCM_BLOCKS; block++) {
                int16_t padded[SX_SPU_ADPCM_SAMPLES_PER_BLOCK] = { 0 };
                for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
                    unsigned at = block * SX_SPU_ADPCM_SAMPLES_PER_BLOCK + i;
                    if (at < SX_OFDM_PACKET_SAMPLES) padded[i] = pcm[at];
                }
                sx_spu_adpcm_encode_block_fast(&enc, padded,
                    block + 1u == ADPCM_BLOCKS ? final_flags : 0, encoded[block]);
                sx_spu_adpcm_decode_block(&dec, encoded[block],
                    restored[channel] + block * SX_SPU_ADPCM_SAMPLES_PER_BLOCK);
            }
        }
        for (unsigned i = 0; i < CHANNEL_SAMPLES; i++) {
            for (unsigned channel = 0; channel < 2; channel++) {
                int16_t clean = 0;
                if (i < SX_OFDM_PACKET_SAMPLES) clean = channel ? right[i] : left[i];
                int16_t out = restored[channel][i];
                int32_t diff = (int32_t)clean - out;
                signal += (uint64_t)((int64_t)clean * clean);
                noise += (uint64_t)((int64_t)diff * diff);
                assert(fwrite(&out, sizeof(out), 1, sf) == 1);
            }
        }
    }
    fclose(sf);

    double adpcm_snr = noise ? 10.0 * log10((double)signal / (double)noise) : 99.0;
    printf("CONTAINER_BYTES=%zu\n", packed);
    printf("CONTAINER_CRC32=%08x\n", container_crc);
    printf("PACKET_COUNT=%zu\n", packet_count);
    printf("GROUP_COUNT=%zu\n", sx_ofdm_group_count(packed));
    printf("SAMPLES_PER_PACKET=%u\n", (unsigned)CHANNEL_SAMPLES);
    printf("TOTAL_FRAMES=%zu\n", packet_count * CHANNEL_SAMPLES);
    printf("PAYLOAD_BYTES=%zu\n", payload_bytes);
    printf("ADPCM_SNR_DB=%.2f\n", adpcm_snr);
    printf("WAV_BYTES=%u\n", data_bytes + 44u);
    printf("STREAM_PATH=%s\n", stream_path);
    printf("CONTAINER_PATH=%s\n", container_path);
    return 0;
}
