/* Feed a captured stereo WAV through src/ofdm_demod.c and check it recovers the
 * OFDM packets (mirrors what tests/decode_wav.js does for the JS worker). */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* pull in the demod as a translation unit */
#ifndef SXD_HOST_TEST
#define SXD_HOST_TEST 1
#endif
#include "../src/ofdm_demod.c"

static float *left, *right;
static long frames;
static uint32_t wav_rate;

static void load_wav(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(2); }
    unsigned char h[12];
    assert(fread(h, 1, 12, f) == 12 && !memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVE", 4));
    unsigned char ch[8];
    int channels = 2, bits = 16; long data_off = -1, data_len = 0;
    while (fread(ch, 1, 8, f) == 8) {
        uint32_t sz = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (!memcmp(ch, "fmt ", 4)) {
            unsigned char fmt[16]; assert(fread(fmt, 1, 16, f) == 16);
            channels = fmt[2] | (fmt[3] << 8);
            wav_rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
            if (sz > 16) fseek(f, sz - 16, SEEK_CUR);
        } else if (!memcmp(ch, "data", 4)) {
            data_off = ftell(f); data_len = sz; fseek(f, sz + (sz & 1), SEEK_CUR);
        } else fseek(f, sz + (sz & 1), SEEK_CUR);
    }
    assert(bits == 16 && data_off >= 0);
    long step = channels;
    frames = data_len / (2 * step);
    left = malloc(sizeof(float) * frames);
    right = malloc(sizeof(float) * frames);
    fseek(f, data_off, SEEK_SET);
    for (long i = 0; i < frames; i++) {
        int16_t s[8];
        assert(fread(s, 2, step, f) == (size_t)step);
        left[i] = s[0] / 32768.0f;
        right[i] = s[step > 1 ? 1 : 0] / 32768.0f;
    }
    fclose(f);
}

int main(int argc, char **argv) {
    const char *wav = argc > 1 ? argv[1] : "/tmp/ps1sx-loopback.wav";
    load_wav(wav);
    long start_frame = argc > 2 ? strtol(argv[2], NULL, 0) : 0;
    assert(start_frame >= 0 && start_frame < frames);

    sxd_reset(0);
    sxd_packet_out_t out[64];
    int total = 0, valid = 0;
    uint32_t image_crc = 0, total_size = 0;
    for (long at = start_frame; at < frames; at += 2048) {
        int n = (int)(frames - at < 2048 ? frames - at : 2048);
        int got = sxd_push(left + at, right + at, n, wav_rate, out, 64);
        for (int i = 0; i < got; i++) {
            total++;
            if (out[i].valid) {
                valid++;
                if (out[i].total_size) { image_crc = out[i].image_crc; total_size = out[i].total_size; }
            }
        }
    }
    printf("start=%ld packets seen=%d  valid=%d  drops=%ld  container=%u bytes  imageCRC=%08x\n",
           start_frame, total, valid, sxd_drops(), total_size, image_crc);
    assert(valid > 0 && "no OFDM packets recovered");
    /* the JS worker recovers every packet from this fixture; require the same */
    assert(valid == total && "some packets failed CRC");
    printf("PASS ofdm_demod (%d/%d packets)\n", valid, total);
    return 0;
}
