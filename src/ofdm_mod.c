#include <stdint.h>
#include <string.h>
#include "ofdm_mod.h"

typedef struct { int32_t re, im; } complex_q12_t;
static complex_q12_t bins[2][SX_OFDM_FFT_SIZE];
static int16_t sync_left[SX_OFDM_FFT_SIZE + SX_OFDM_CP_SIZE];
static int16_t sync_right[SX_OFDM_FFT_SIZE + SX_OFDM_CP_SIZE];
static uint8_t sync_ready;
static const int16_t qam16_levels[4] = { -3885, -1295, 3885, 1295 };

static int is_pilot(unsigned carrier) {
    return carrier == 0 || carrier == 13 || carrier == 27 || carrier == 41 ||
           carrier == 55 || carrier == 69 || carrier == 83 || carrier == 95;
}

static int32_t clamp16(int32_t value) {
    if (value < -32768) return -32768;
    if (value > 32767) return 32767;
    return value;
}

/* Self-contained fixed-point sine, bit-identical on the host and the PS1 (no
 * libm, no SDK trig).  Hard-coded Q15 quarter-wave table (257 entries, index
 * k -> sin(k/256 * pi/2)); angle unit is 4194304 == 2*pi, so (angle >> 12)
 * gives a 0..1023 position around the circle. */
static const int16_t sine_quarter[257] = {
    0, 201, 402, 603, 804, 1005, 1206, 1407, 1608, 1809, 2009, 2210,
    2410, 2611, 2811, 3012, 3212, 3412, 3612, 3811, 4011, 4210, 4410, 4609,
    4808, 5007, 5205, 5404, 5602, 5800, 5998, 6195, 6393, 6590, 6786, 6983,
    7179, 7375, 7571, 7767, 7962, 8157, 8351, 8545, 8739, 8933, 9126, 9319,
    9512, 9704, 9896, 10087, 10278, 10469, 10659, 10849, 11039, 11228, 11417, 11605,
    11793, 11980, 12167, 12353, 12539, 12725, 12910, 13094, 13279, 13462, 13645, 13828,
    14010, 14191, 14372, 14553, 14732, 14912, 15090, 15269, 15446, 15623, 15800, 15976,
    16151, 16325, 16499, 16673, 16846, 17018, 17189, 17360, 17530, 17700, 17869, 18037,
    18204, 18371, 18537, 18703, 18868, 19032, 19195, 19357, 19519, 19680, 19841, 20000,
    20159, 20317, 20475, 20631, 20787, 20942, 21096, 21250, 21403, 21554, 21705, 21856,
    22005, 22154, 22301, 22448, 22594, 22739, 22884, 23027, 23170, 23311, 23452, 23592,
    23731, 23870, 24007, 24143, 24279, 24413, 24547, 24680, 24811, 24942, 25072, 25201,
    25329, 25456, 25582, 25708, 25832, 25955, 26077, 26198, 26319, 26438, 26556, 26674,
    26790, 26905, 27019, 27133, 27245, 27356, 27466, 27575, 27683, 27790, 27896, 28001,
    28105, 28208, 28310, 28411, 28510, 28609, 28706, 28803, 28898, 28992, 29085, 29177,
    29268, 29358, 29447, 29534, 29621, 29706, 29791, 29874, 29956, 30037, 30117, 30195,
    30273, 30349, 30424, 30498, 30571, 30643, 30714, 30783, 30852, 30919, 30985, 31050,
    31113, 31176, 31237, 31297, 31356, 31414, 31470, 31526, 31580, 31633, 31685, 31736,
    31785, 31833, 31880, 31926, 31971, 32014, 32057, 32098, 32137, 32176, 32213, 32250,
    32285, 32318, 32351, 32382, 32412, 32441, 32469, 32495, 32521, 32545, 32567, 32589,
    32609, 32628, 32646, 32663, 32678, 32692, 32705, 32717, 32728, 32737, 32745, 32752,
    32757, 32761, 32765, 32766, 32767
};

static int32_t sine_q15(unsigned angle) {
    unsigned idx = (angle >> 12) & 0x3FF;   /* 1024 steps per circle */
    unsigned quad = idx >> 8, pos = idx & 0xFF;
    switch (quad) {
        case 0:  return  sine_quarter[pos];
        case 1:  return  sine_quarter[256 - pos];
        case 2:  return -sine_quarter[pos];
        default: return -sine_quarter[256 - pos];
    }
}

static int32_t sine(unsigned angle) { return (sine_q15(angle) * 4096) >> 15; }
static int32_t cosine(unsigned angle) { return sine(angle + 1048576u); } /* +90 deg */

static void ifft(complex_q12_t *values) {
    for (unsigned i = 1, j = 0; i < SX_OFDM_FFT_SIZE; i++) {
        unsigned bit = SX_OFDM_FFT_SIZE >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { complex_q12_t tmp = values[i]; values[i] = values[j]; values[j] = tmp; }
    }
    for (unsigned length = 2; length <= SX_OFDM_FFT_SIZE; length <<= 1) {
        unsigned half = length >> 1, step = 4194304u / length;
        for (unsigned j = 0; j < half; j++) {
            int32_t wr = cosine(j * step), wi = sine(j * step);
            for (unsigned base = 0; base < SX_OFDM_FFT_SIZE; base += length) {
                complex_q12_t odd = values[base + j + half], even = values[base + j];
                /* Plain Q12 fixed point, identical on host and PS1.  The earlier
                 * GTE path truncated odd.re/odd.im to int16, corrupting the
                 * transform for the higher-amplitude symbols; wr,wi <= 4096 and
                 * |odd| stays well under 2^18 here so int32 products never
                 * overflow. */
                int32_t tr = (wr * odd.re - wi * odd.im) >> 12;
                int32_t ti = (wr * odd.im + wi * odd.re) >> 12;
                values[base + j].re = (even.re + tr) >> 1;
                values[base + j].im = (even.im + ti) >> 1;
                values[base + j + half].re = (even.re - tr) >> 1;
                values[base + j + half].im = (even.im - ti) >> 1;
            }
        }
    }
}

static void set_bin(unsigned channel, unsigned bin, int32_t re, int32_t im) {
    bins[channel][bin].re = re; bins[channel][bin].im = im;
    bins[channel][SX_OFDM_FFT_SIZE - bin].re = re;
    bins[channel][SX_OFDM_FFT_SIZE - bin].im = -im;
}

static void render_symbol(int16_t *left, int16_t *right) {
    /* Pack two conjugate-symmetric spectra into one complex IFFT:
     * IFFT(X + iY) = left + i*right. This halves the FFT workload. */
    for (unsigned i = 0; i < SX_OFDM_FFT_SIZE; i++) {
        int32_t xr = bins[0][i].re, xi = bins[0][i].im;
        int32_t yr = bins[1][i].re, yi = bins[1][i].im;
        bins[0][i].re = xr - yi;
        bins[0][i].im = xi + yr;
    }
    ifft(bins[0]);
    for (unsigned i = 0; i < SX_OFDM_FFT_SIZE + SX_OFDM_CP_SIZE; i++) {
        unsigned source = (i + SX_OFDM_FFT_SIZE - SX_OFDM_CP_SIZE) & (SX_OFDM_FFT_SIZE - 1);
        left[i] = (int16_t)clamp16(bins[0][source].re * 32);
        right[i] = (int16_t)clamp16(bins[0][source].im * 32);
    }
}

static void whiten(const uint8_t *source, uint8_t *target, uint32_t index) {
    uint32_t state = 0x9e3779b9u ^ index;
    for (unsigned i = 0; i < SX_OFDM_WIRE_BYTES; i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        target[i] = source[i] ^ (uint8_t)state;
    }
}

static void ensure_sync(void) {
    if (sync_ready) return;
    memset(bins, 0, sizeof(bins));
    for (unsigned i = 0; i < SX_OFDM_CARRIERS; i++) {
        unsigned bin = SX_OFDM_FIRST_BIN + i;
        int32_t level = ((bin * 73u + 19u) & 1) ? -4096 : 4096;
        set_bin(0, bin, level, 0); set_bin(1, bin, level, 0);
    }
    render_symbol(sync_left, sync_right);
    sync_ready = 1;
}

void sx_ofdm_modulate_packet(const uint8_t packet[SX_OFDM_PACKET_BYTES], uint32_t packet_index,
                             int16_t left[SX_OFDM_PACKET_SAMPLES], int16_t right[SX_OFDM_PACKET_SAMPLES]) {
    uint8_t protected_packet[SX_OFDM_WIRE_BYTES], scrambled[SX_OFDM_WIRE_BYTES];
    sx_inner_fec_encode(packet, protected_packet); whiten(protected_packet, scrambled, packet_index);
    ensure_sync();
    memcpy(left, sync_left, sizeof(sync_left));
    memcpy(right, sync_right, sizeof(sync_right));
    for (unsigned symbol = 0; symbol < 4; symbol++) {
        memset(bins, 0, sizeof(bins)); unsigned data_index = 0;
        for (unsigned carrier = 0; carrier < SX_OFDM_CARRIERS; carrier++) {
            unsigned bin = SX_OFDM_FIRST_BIN + carrier;
            if (is_pilot(carrier)) {
                int32_t pilot = ((packet_index + symbol) & 1) ? -4096 : 4096;
                set_bin(0, bin, pilot, 0); set_bin(1, bin, pilot, 0);
            } else {
                /* One byte per carrier: each stereo channel carries one
                 * 16-QAM nibble (I/Q = two bits each). */
                unsigned codes = scrambled[symbol * SX_OFDM_DATA_CARRIERS + data_index];
                for (unsigned channel = 0; channel < 2; channel++) {
                    unsigned code = (codes >> (channel * 4u)) & 15u;
                    set_bin(channel, bin, qam16_levels[code & 3u], qam16_levels[(code >> 2) & 3u]);
                }
                data_index++;
            }
        }
        unsigned offset = (symbol + 1) * (SX_OFDM_FFT_SIZE + SX_OFDM_CP_SIZE);
        render_symbol(left + offset, right + offset);
    }
}

void sx_ofdm_modulate_packet_mono(const uint8_t packet[SX_OFDM_PACKET_BYTES], uint32_t packet_index,
                                  int16_t left[SX_OFDM_MONO_PACKET_SAMPLES],
                                  int16_t right[SX_OFDM_MONO_PACKET_SAMPLES]) {
    uint8_t protected_packet[SX_OFDM_WIRE_BYTES], scrambled[SX_OFDM_WIRE_BYTES];
    sx_inner_fec_encode(packet, protected_packet); whiten(protected_packet, scrambled, packet_index);
    ensure_sync();
    memcpy(left, sync_left, sizeof(sync_left));
    memcpy(right, sync_left, sizeof(sync_left));
    for (unsigned symbol = 0; symbol < 8; symbol++) {
        memset(bins, 0, sizeof(bins));
        unsigned data_index = 0;
        for (unsigned carrier = 0; carrier < SX_OFDM_CARRIERS; carrier++) {
            unsigned bin = SX_OFDM_FIRST_BIN + carrier;
            if (is_pilot(carrier)) {
                int32_t pilot = ((packet_index + symbol) & 1) ? -4096 : 4096;
                set_bin(0, bin, pilot, 0);
            } else {
                unsigned code_index = symbol * SX_OFDM_DATA_CARRIERS + data_index;
                unsigned code = (scrambled[code_index >> 1] >> ((code_index & 1u) * 4u)) & 15u;
                set_bin(0, bin, qam16_levels[code & 3u], qam16_levels[(code >> 2) & 3u]);
                data_index++;
            }
        }
        unsigned offset = (symbol + 1u) * (SX_OFDM_FFT_SIZE + SX_OFDM_CP_SIZE);
        render_symbol(left + offset, right + offset);
        memcpy(right + offset, left + offset,
               (SX_OFDM_FFT_SIZE + SX_OFDM_CP_SIZE) * sizeof(int16_t));
    }
}
