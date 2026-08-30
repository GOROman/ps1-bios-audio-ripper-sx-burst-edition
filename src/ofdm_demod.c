/* OFDM receiver DSP, ported from web/ofdm-worker.js so the browser can run it as
 * WebAssembly (freestanding wasm32) and the host can unit-test it.
 *
 * Pipeline: push captured stereo PCM at any rate -> windowed-sinc resample to
 * 44.1 kHz -> sync search -> per-packet FFT demod -> QPSK slice -> de-whiten ->
 * packet CRC.  Group assembly / container rebuild stay in JS.
 *
 * No libc: math comes from builtins and small local approximations, buffers are
 * static.  Build for the host with -DSXD_HOST_TEST for libm + a main-less object.
 */
#include <stdint.h>
#include <stddef.h>

#define SXD_PI 3.14159265358979323846
#define FEC_PARITY_SHARDS 4u

#ifdef SXD_HOST_TEST
#include <math.h>
#define SXD_SIN sin
#define SXD_COS cos
#define SXD_SQRT sqrt
#define SXD_ATAN2 atan2
#define SXD_LOG10 log10
#else
/* ---- freestanding math ---- */
static double sxd_fabs(double x) { return x < 0 ? -x : x; }
static double SXD_SQRT(double x) { return __builtin_sqrt(x); }
/* Range-reduced 11th-order sine (|err| < 2e-9 on [-pi,pi]). */
static double sxd_sin_core(double x) {
    double x2 = x * x;
    return x * (1.0 + x2 * (-1.66666666666666324348e-1 + x2 * (8.33333333332248946124e-3 +
           x2 * (-1.98412698298579493134e-4 + x2 * (2.75573137070700676789e-6 +
           x2 * (-2.50507602534068634195e-8 + x2 * 1.58969099521155010221e-10))))));
}
static double SXD_SIN(double x) {
    /* reduce to [-pi, pi] */
    double k = x * (1.0 / (2.0 * SXD_PI));
    k = k >= 0 ? (double)(long long)(k + 0.5) : (double)(long long)(k - 0.5);
    x -= k * (2.0 * SXD_PI);
    if (x > SXD_PI) x -= 2.0 * SXD_PI;
    else if (x < -SXD_PI) x += 2.0 * SXD_PI;
    /* fold to [-pi/2, pi/2] for accuracy */
    if (x > SXD_PI / 2) x = SXD_PI - x;
    else if (x < -SXD_PI / 2) x = -SXD_PI - x;
    return sxd_sin_core(x);
}
static double SXD_COS(double x) { return SXD_SIN(x + SXD_PI / 2); }
static double sxd_atan_core(double z) {
    /* atan on [-1,1], minimax-ish, |err| < 1e-5 (enough for pilot phase) */
    double z2 = z * z;
    return z * (0.99997726 + z2 * (-0.33262347 + z2 * (0.19354346 +
           z2 * (-0.11643287 + z2 * (0.05265332 + z2 * -0.01172120)))));
}
static double SXD_ATAN2(double y, double x) {
    double ax = sxd_fabs(x), ay = sxd_fabs(y), a, r;
    if (ax == 0.0 && ay == 0.0) return 0.0;
    if (ax >= ay) { a = ay / ax; r = sxd_atan_core(a); }
    else { a = ax / ay; r = SXD_PI / 2 - sxd_atan_core(a); }
    if (x < 0) r = SXD_PI - r;
    if (y < 0) r = -r;
    return r;
}
static double SXD_LOG10(double x) {
    /* only used for the diagnostic SNR field; crude is fine */
    if (x <= 0) return -99.0;
    int e = 0;
    while (x > 10.0) { x /= 10.0; e++; }
    while (x < 1.0) { x *= 10.0; e--; }
    double t = (x - 1) / (x + 1), t2 = t * t;
    double ln = 2 * t * (1 + t2 * (1.0 / 3 + t2 * (1.0 / 5 + t2 / 7)));
    return e + ln * 0.4342944819032518;
}
#endif

#define RATE 44100
#define FFT 512
#define CP 64
#define SYMBOL (FFT + CP)
#define PACKET_SAMPLES (SYMBOL * 5)
#define MONO_PACKET_SAMPLES (SYMBOL * 9)
#define FIRST_BIN 24
#define CARRIERS 96
#define DATA_CARRIERS 88
#define PAYLOAD 144
#define PACKET_BYTES 176
#define MAGIC 0x314f5853u
#define LOCK_WINDOW 160
#define AUDIO_CAP 200000
#define RS_HALF 12

static const int PILOT_BINS[8] = { 24, 37, 51, 65, 79, 93, 107, 119 };
static int is_pilot_carrier(int c) {
    return c == 0 || c == 13 || c == 27 || c == 41 || c == 55 || c == 69 || c == 83 || c == 95;
}
static const double QLEV[2] = { -0.70710678118654752, 0.70710678118654752 };

/* ---- CRC32 (reflected, poly 0xedb88320) ---- */
static uint32_t sxd_crc32(const uint8_t *data, int len) {
    uint32_t crc = 0xffffffffu;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)(-(int32_t)(crc & 1)));
    }
    return crc ^ 0xffffffffu;
}

/* CRC-guided hard-decision repair.  A marginal QPSK decision commonly damages
 * just one of the 1408 packet bits.  Trying each bit is cheap on the browser
 * worker and turns that detectable error into a uniquely validated packet
 * without changing the wire format or weakening CRC acceptance. */
static int repair_single_bit(uint8_t packet[PACKET_BYTES], uint32_t *sent_out,
                             uint32_t *computed_out) {
    uint8_t tmp[PACKET_BYTES];
    for (int bit = 0; bit < PACKET_BYTES * 8; bit++) {
        int byte = bit >> 3;
        uint8_t mask = (uint8_t)(1u << (bit & 7));
        packet[byte] ^= mask;
        uint32_t magic = (uint32_t)packet[0] | ((uint32_t)packet[1] << 8) |
                         ((uint32_t)packet[2] << 16) | ((uint32_t)packet[3] << 24);
        if (magic == MAGIC && packet[4] == 3) {
            uint32_t sent = (uint32_t)packet[28] | ((uint32_t)packet[29] << 8) |
                            ((uint32_t)packet[30] << 16) | ((uint32_t)packet[31] << 24);
            for (int i = 0; i < PACKET_BYTES; i++) tmp[i] = packet[i];
            tmp[28] = tmp[29] = tmp[30] = tmp[31] = 0;
            uint32_t computed = sxd_crc32(tmp, PACKET_BYTES);
            if (sent == computed) {
                *sent_out = sent;
                *computed_out = computed;
                return 1;
            }
        }
        packet[byte] ^= mask;
    }
    return 0;
}

/* ---- state ---- */
static double audioL[AUDIO_CAP], audioR[AUDIO_CAP];
static int audio_len;
static double sync_tpl[SYMBOL];
static int armed, locked, carrier_reported;
static uint32_t expected_packet;
static long received_samples, report_at, drops;
static int crc_ok, crc_bad;
static int mono_mode, mono_channel;

/* resampler */
static double rs_buf_l[AUDIO_CAP], rs_buf_r[AUDIO_CAP];
static long rs_base;
static int rs_len;
static double rs_pos;
static double input_rate = RATE;

static void build_sync(void) {
    double power = 0;
    for (int n = -CP; n < FFT; n++) {
        double v = 0; int at = (n + FFT) & (FFT - 1);
        for (int c = 0; c < CARRIERS; c++) {
            int k = FIRST_BIN + c;
            double s = ((k * 73 + 19) & 1) ? -1.0 : 1.0;
            v += 2.0 * s * SXD_COS(2.0 * SXD_PI * k * at / FFT);
        }
        sync_tpl[n + CP] = v; power += v * v;
    }
    power = SXD_SQRT(power);
    for (int i = 0; i < SYMBOL; i++) sync_tpl[i] /= power;
}

/* iterative radix-2 DIT FFT, in place (matches the JS worker's fft()) */
static void fft(double *re, double *im) {
    int n = FFT;
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2.0 * SXD_PI / len;
        for (int base = 0; base < n; base += len) {
            for (int k = 0; k < len / 2; k++) {
                double wr = SXD_COS(ang * k), wi = SXD_SIN(ang * k);
                int at = base + k + len / 2, bt = base + k;
                double ar = re[at] * wr - im[at] * wi;
                double ai = re[at] * wi + im[at] * wr;
                double br = re[bt], bi = im[bt];
                re[bt] = br + ar; im[bt] = bi + ai;
                re[at] = br - ar; im[at] = bi - ai;
            }
        }
    }
}

/* ---- whiten (xorshift32, seed 0x9e3779b9 ^ index) ---- */
static void whiten(const uint8_t *src, uint8_t *dst, uint32_t index) {
    uint32_t state = 0x9e3779b9u ^ index;
    for (int i = 0; i < PACKET_BYTES; i++) {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        dst[i] = src[i] ^ (uint8_t)state;
    }
}

/* Offline captures can contain several independently framed OFDM bursts, so
 * their whitening index need not continue from the previous burst.  Probe
 * same-parity indices (pilot polarity is parity-dependent) and accept only a
 * complete magic/version/CRC match. */
static int probe_whitening_index(const uint8_t *scrambled, uint32_t parity,
                                 uint8_t *packet, uint32_t *sent_out, uint32_t *computed_out) {
    uint8_t candidate[PACKET_BYTES], tmp[PACKET_BYTES];
    for (uint32_t index = parity & 1u; index < 4096u; index += 2u) {
        uint32_t state = 0x9e3779b9u ^ index;
        for (int i = 0; i < 5; i++) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            candidate[i] = scrambled[i] ^ (uint8_t)state;
        }
        uint32_t magic = (uint32_t)candidate[0] | ((uint32_t)candidate[1] << 8) |
                         ((uint32_t)candidate[2] << 16) | ((uint32_t)candidate[3] << 24);
        if (magic != MAGIC || candidate[4] != 3) continue;
        for (int i = 5; i < PACKET_BYTES; i++) {
            state ^= state << 13; state ^= state >> 17; state ^= state << 5;
            candidate[i] = scrambled[i] ^ (uint8_t)state;
        }
        uint32_t sent = (uint32_t)candidate[28] | ((uint32_t)candidate[29] << 8) |
                        ((uint32_t)candidate[30] << 16) | ((uint32_t)candidate[31] << 24);
        for (int i = 0; i < PACKET_BYTES; i++) tmp[i] = candidate[i];
        tmp[28] = tmp[29] = tmp[30] = tmp[31] = 0;
        uint32_t computed = sxd_crc32(tmp, PACKET_BYTES);
        if (sent == computed) {
            for (int i = 0; i < PACKET_BYTES; i++) packet[i] = candidate[i];
            *sent_out = sent; *computed_out = computed;
            return 1;
        }
    }
    return 0;
}

/* ---- windowed-sinc resampler ---- */
static double win_sinc(double x, double cutoff) {
    if (x <= -RS_HALF || x >= RS_HALF) return 0;
    double px = SXD_PI * x;
    double s = x == 0 ? cutoff : SXD_SIN(cutoff * px) / px;
    double w = 0.42 + 0.5 * SXD_COS(px / RS_HALF) + 0.08 * SXD_COS(2 * px / RS_HALF);
    return s * w;
}

static void audio_push(double l, double r) {
    if (audio_len < AUDIO_CAP) { audioL[audio_len] = l; audioR[audio_len] = r; audio_len++; }
}

static void resample_push(const float *left, const float *right, int frames, double rate) {
    input_rate = rate > 0 ? rate : input_rate;
    if (input_rate < RATE + 0.5 && input_rate > RATE - 0.5) {
        for (int i = 0; i < frames; i++) audio_push(left[i], right ? right[i] : left[i]);
        return;
    }
    double step = input_rate / RATE, cutoff = RATE < input_rate ? RATE / input_rate : 1.0;
    for (int i = 0; i < frames && rs_len < AUDIO_CAP; i++) {
        rs_buf_l[rs_len] = left[i];
        rs_buf_r[rs_len] = right ? right[i] : left[i];
        rs_len++;
    }
    long avail = rs_base + rs_len;
    while (rs_pos + RS_HALF < avail) {
        long c0 = (long)rs_pos; double frac = rs_pos - c0;
        double accl = 0, accr = 0, norm = 0;
        for (int t = -RS_HALF + 1; t <= RS_HALF; t++) {
            long idx = c0 + t - rs_base;
            if (idx < 0 || idx >= rs_len) continue;
            double k = win_sinc(t - frac, cutoff);
            accl += rs_buf_l[idx] * k; accr += rs_buf_r[idx] * k; norm += k;
        }
        if (norm == 0) norm = 1;
        audio_push(accl / norm, accr / norm);
        rs_pos += step;
    }
    long keep = (long)rs_pos - RS_HALF - 1;
    if (keep > rs_base) {
        long d = keep - rs_base;
        for (int i = 0; i + d < rs_len; i++) { rs_buf_l[i] = rs_buf_l[i + d]; rs_buf_r[i] = rs_buf_r[i + d]; }
        rs_len -= (int)d; rs_base += d;
    }
}

/* ---- sync search ---- */
static double correlation(int at) {
    double dl = 0, dr = 0, pl = 0, pr = 0;
    for (int i = 0; i < SYMBOL; i++) {
        double l = audioL[at + i], r = audioR[at + i], s = sync_tpl[i];
        dl += l * s; dr += r * s; pl += l * l; pr += r * r;
    }
    double cl = (dl < 0 ? -dl : dl) / SXD_SQRT(pl > 0 ? pl : 1);
    double cr = (dr < 0 ? -dr : dr) / SXD_SQRT(pr > 0 ? pr : 1);
    return cl > cr ? cl : cr;
}

static int scan_range(int lo, int hi, int stepv, double *out_score) {
    int best_at = lo; double best = -1;
    for (int at = lo; at <= hi; at += stepv) {
        double v = correlation(at);
        if (v > best) { best = v; best_at = at; }
        if (v >= 0.6) {
            int a0 = at - stepv < lo ? lo : at - stepv, a1 = at + stepv > hi ? hi : at + stepv;
            for (int a = a0; a <= a1; a++) { double rv = correlation(a); if (rv > best) { best = rv; best_at = a; } }
            *out_score = best; return best_at;
        }
    }
    *out_score = best; return best_at;
}

/* returns sync sample offset, or -1 */
static int find_sync(double *out_score) {
    if (audio_len < PACKET_SAMPLES + 4) return -1;
    int end = audio_len - PACKET_SAMPLES - 4; if (end > 4096) end = 4096;
    double sc;
    if (locked) {
        int w = end < LOCK_WINDOW ? end : LOCK_WINDOW;
        int a = scan_range(0, w, 1, &sc);
        if (sc >= 0.5) { *out_score = sc; return a; }
        locked = 0;
    }
    int at = scan_range(0, end, 2, &sc);
    if (sc < 0.42) {
        if (audio_len > PACKET_SAMPLES + 8192) {
            int drop = audio_len - (PACKET_SAMPLES + 8192);
            for (int i = 0; i + drop < audio_len; i++) { audioL[i] = audioL[i + drop]; audioR[i] = audioR[i + drop]; }
            audio_len -= drop; drops++;
        }
        return -1;
    }
    double best = sc; int bat = at;
    int a0 = at - 4 < 0 ? 0 : at - 4, a1 = at + 4 > end ? end : at + 4;
    for (int a = a0; a <= a1; a++) { double v = correlation(a); if (v > best) { best = v; bat = a; } }
    *out_score = best; return bat;
}

/* ---- small helpers ---- */
static void unwrap8(double *v) {
    for (int i = 1; i < 8; i++) {
        while (v[i] - v[i - 1] > SXD_PI) v[i] -= 2 * SXD_PI;
        while (v[i] - v[i - 1] < -SXD_PI) v[i] += 2 * SXD_PI;
    }
}
static void fit_line(const int *xs, const double *ys, int n, double *slope, double *intercept) {
    double mx = 0, my = 0;
    for (int i = 0; i < n; i++) { mx += xs[i]; my += ys[i]; }
    mx /= n; my /= n;
    double top = 0, bot = 0;
    for (int i = 0; i < n; i++) { top += (xs[i] - mx) * (ys[i] - my); bot += (xs[i] - mx) * (xs[i] - mx); }
    *slope = bot != 0 ? top / bot : 0;
    *intercept = my - *slope * mx;
}
static double median8(double *m) {
    for (int i = 0; i < 8; i++) for (int j = i + 1; j < 8; j++) if (m[j] < m[i]) { double t = m[i]; m[i] = m[j]; m[j] = t; }
    return (m[3] + m[4]) / 2;
}

/* ---- decode one packet at sample offset `at` ---- */
typedef struct {
    uint8_t packet[PACKET_BYTES];
    int valid;
    uint32_t sent, computed;
    double score, evm, snr, phase, timing;
    double carrier_error[CARRIERS];
    int carrier_count[CARRIERS];
    int swap;
} sxd_result_t;

static void estimate_channel(const double *source, int at, double *hr, double *hi) {
    for (int i = 0; i < FFT; i++) { hr[i] = source[at + CP + i]; hi[i] = 0; }
    fft(hr, hi);
    for (int carrier = 0; carrier < CARRIERS; carrier++) {
        int bin = FIRST_BIN + carrier;
        double sign = ((bin * 73 + 19) & 1) ? -1.0 : 1.0;
        hr[bin] *= sign;
        hi[bin] *= sign;
    }
}

static void equalize_carriers(const double *source, int base,
                              const double *hr, const double *hi,
                              double *er, double *ei) {
    double re[FFT], im[FFT];
    for (int i = 0; i < FFT; i++) { re[i] = source[base + i]; im[i] = 0; }
    fft(re, im);
    for (int carrier = 0; carrier < CARRIERS; carrier++) {
        int bin = FIRST_BIN + carrier;
        double den = hr[bin] * hr[bin] + hi[bin] * hi[bin];
        if (den < 1e-18) den = 1e-18;
        er[bin] = (re[bin] * hr[bin] + im[bin] * hi[bin]) / den;
        ei[bin] = (im[bin] * hr[bin] - re[bin] * hi[bin]) / den;
    }
}

static void decode_packet(int at, uint32_t index, double score, int swap, sxd_result_t *out) {
    uint8_t nibbles[352]; int na = 0;
    double channel_r[2][FFT], channel_i[2][FFT];
    for (int ch = 0; ch < 2; ch++) {
        const double *source = (ch ^ (swap ? 1 : 0)) ? audioR : audioL;
        estimate_channel(source, at, channel_r[ch], channel_i[ch]);
    }
    for (int carrier = 0; carrier < CARRIERS; carrier++) {
        out->carrier_error[carrier] = 0;
        out->carrier_count[carrier] = 0;
    }
    double evm_sum = 0, phase_sum = 0, timing_sum = 0; int evm_cnt = 0;
    for (int symbol = 0; symbol < 4; symbol++) {
        int codes_all[2][CARRIERS];
        for (int ch = 0; ch < 2; ch++) {
            const double *source = (ch ^ (swap ? 1 : 0)) ? audioR : audioL;
            double re[FFT] = {0}, im[FFT] = {0};
            int base = at + (symbol + 1) * SYMBOL + CP;
            equalize_carriers(source, base, channel_r[ch], channel_i[ch], re, im);
            double expv = ((index + symbol) & 1) ? -1.0 : 1.0;
            double angles[8], mags[8];
            for (int p = 0; p < 8; p++) {
                int bin = PILOT_BINS[p];
                angles[p] = SXD_ATAN2(im[bin] * expv, re[bin] * expv);
                mags[p] = SXD_SQRT(re[bin] * re[bin] + im[bin] * im[bin]);
            }
            unwrap8(angles);
            double slope, intercept;
            fit_line(PILOT_BINS, angles, 8, &slope, &intercept);
            double gain = median8(mags); if (gain < 1e-12) gain = 1e-12;
            phase_sum += intercept;
            timing_sum += -slope * FFT / (2 * SXD_PI);
            for (int carrier = 0; carrier < CARRIERS; carrier++) {
                if (is_pilot_carrier(carrier)) { codes_all[ch][carrier] = -1; continue; }
                int bin = FIRST_BIN + carrier;
                double phase = intercept + slope * bin;
                double c = SXD_COS(-phase), s = SXD_SIN(-phase);
                double zr = (re[bin] * c - im[bin] * s) / gain;
                double zi = (re[bin] * s + im[bin] * c) / gain;
                int ri = zr >= 0 ? 1 : 0, ii = zi >= 0 ? 1 : 0;
                codes_all[ch][carrier] = ri | (ii << 1);
                double error = (zr - QLEV[ri]) * (zr - QLEV[ri]) + (zi - QLEV[ii]) * (zi - QLEV[ii]);
                evm_sum += error;
                out->carrier_error[carrier] += error;
                out->carrier_count[carrier]++;
                evm_cnt++;
            }
        }
        int idx0 = 0, idx1 = 0;
        for (int i = 0; i < 88; i++) {
            while (codes_all[0][idx0] < 0) idx0++;
            while (codes_all[1][idx1] < 0) idx1++;
            nibbles[na++] = (uint8_t)(codes_all[0][idx0++] | (codes_all[1][idx1++] << 2));
        }
    }
    uint8_t scrambled[PACKET_BYTES];
    for (int i = 0; i < PACKET_BYTES; i++) scrambled[i] = (uint8_t)(nibbles[i * 2] | (nibbles[i * 2 + 1] << 4));
    whiten(scrambled, out->packet, index);
    uint32_t sent = (uint32_t)out->packet[28] | ((uint32_t)out->packet[29] << 8) |
                    ((uint32_t)out->packet[30] << 16) | ((uint32_t)out->packet[31] << 24);
    uint8_t tmp[PACKET_BYTES];
    for (int i = 0; i < PACKET_BYTES; i++) tmp[i] = out->packet[i];
    tmp[28] = tmp[29] = tmp[30] = tmp[31] = 0;
    uint32_t computed = sxd_crc32(tmp, PACKET_BYTES);
    uint32_t magic = (uint32_t)out->packet[0] | ((uint32_t)out->packet[1] << 8) |
                     ((uint32_t)out->packet[2] << 16) | ((uint32_t)out->packet[3] << 24);
    double evm = SXD_SQRT(evm_sum / (evm_cnt > 0 ? evm_cnt : 1));
    out->valid = magic == MAGIC && out->packet[4] == 3 && sent == computed;
    if (!out->valid && repair_single_bit(out->packet, &sent, &computed))
        out->valid = 1;
    if (!out->valid && probe_whitening_index(scrambled, index, out->packet, &sent, &computed))
        out->valid = 1;
    out->sent = sent; out->computed = computed;
    out->score = score; out->evm = evm;
    out->snr = -20.0 * SXD_LOG10(evm > 1e-9 ? evm : 1e-9);
    out->phase = phase_sum / 4; out->timing = timing_sum / 4; out->swap = swap;
}

/* Mono packets carry the same 704 QPSK dibits over eight data symbols.  The
 * sync symbol occupies every carrier, so it also gives a per-carrier complex
 * channel estimate.  Dividing by that estimate removes analogue frequency
 * response and phase distortion before the pilots correct residual drift. */
static void decode_mono_packet(int at, uint32_t index, double score, int channel, sxd_result_t *out) {
    double hr[2][FFT], hi[2][FFT];
    estimate_channel(audioL, at, hr[0], hi[0]);
    estimate_channel(audioR, at, hr[1], hi[1]);

    uint8_t codes[704]; int ca = 0;
    for (int c = 0; c < CARRIERS; c++) { out->carrier_error[c] = 0; out->carrier_count[c] = 0; }
    double evm_sum = 0, phase_sum = 0, timing_sum = 0; int evm_cnt = 0;
    for (int symbol = 0; symbol < 8; symbol++) {
        double eqr[2][FFT] = {{0}}, eqi[2][FFT] = {{0}};
        int base = at + (symbol + 1) * SYMBOL + CP;
        equalize_carriers(audioL, base, hr[0], hi[0], eqr[0], eqi[0]);
        equalize_carriers(audioR, base, hr[1], hi[1], eqr[1], eqi[1]);
        double expv = ((index + symbol) & 1) ? -1.0 : 1.0;
        double angles[8], mags[8];
        for (int p = 0; p < 8; p++) {
            int bin = PILOT_BINS[p];
            double power[2];
            for (int ch = 0; ch < 2; ch++) power[ch] = hr[ch][bin] * hr[ch][bin] + hi[ch][bin] * hi[ch][bin];
            double den = power[0] + power[1]; if (den < 1e-18) den = 1e-18;
            double zr = (eqr[0][bin] * power[0] + eqr[1][bin] * power[1]) / den;
            double zi = (eqi[0][bin] * power[0] + eqi[1][bin] * power[1]) / den;
            angles[p] = SXD_ATAN2(zi * expv, zr * expv);
            mags[p] = SXD_SQRT(zr * zr + zi * zi);
        }
        unwrap8(angles);
        double slope, intercept;
        fit_line(PILOT_BINS, angles, 8, &slope, &intercept);
        double gain = median8(mags); if (gain < 1e-12) gain = 1e-12;
        phase_sum += intercept; timing_sum += -slope * FFT / (2 * SXD_PI);
        for (int carrier = 0; carrier < CARRIERS; carrier++) {
            if (is_pilot_carrier(carrier)) continue;
            int bin = FIRST_BIN + carrier;
            double power[2];
            for (int ch = 0; ch < 2; ch++) power[ch] = hr[ch][bin] * hr[ch][bin] + hi[ch][bin] * hi[ch][bin];
            double den = power[0] + power[1]; if (den < 1e-18) den = 1e-18;
            double mr = (eqr[0][bin] * power[0] + eqr[1][bin] * power[1]) / den;
            double mi = (eqi[0][bin] * power[0] + eqi[1][bin] * power[1]) / den;
            double phase = intercept + slope * bin;
            double c = SXD_COS(-phase), s = SXD_SIN(-phase);
            double zr = (mr * c - mi * s) / gain;
            double zi = (mr * s + mi * c) / gain;
            int ri = zr >= 0 ? 1 : 0, ii = zi >= 0 ? 1 : 0;
            codes[ca++] = (uint8_t)(ri | (ii << 1));
            double error = (zr - QLEV[ri]) * (zr - QLEV[ri]) + (zi - QLEV[ii]) * (zi - QLEV[ii]);
            evm_sum += error;
            out->carrier_error[carrier] += error;
            out->carrier_count[carrier]++;
            evm_cnt++;
        }
    }
    uint8_t scrambled[PACKET_BYTES];
    for (int i = 0; i < PACKET_BYTES; i++)
        scrambled[i] = (uint8_t)(codes[i * 4] | (codes[i * 4 + 1] << 2) |
                                 (codes[i * 4 + 2] << 4) | (codes[i * 4 + 3] << 6));
    whiten(scrambled, out->packet, index);
    uint32_t sent = (uint32_t)out->packet[28] | ((uint32_t)out->packet[29] << 8) |
                    ((uint32_t)out->packet[30] << 16) | ((uint32_t)out->packet[31] << 24);
    uint8_t tmp[PACKET_BYTES];
    for (int i = 0; i < PACKET_BYTES; i++) tmp[i] = out->packet[i];
    tmp[28] = tmp[29] = tmp[30] = tmp[31] = 0;
    uint32_t computed = sxd_crc32(tmp, PACKET_BYTES);
    uint32_t magic = (uint32_t)out->packet[0] | ((uint32_t)out->packet[1] << 8) |
                     ((uint32_t)out->packet[2] << 16) | ((uint32_t)out->packet[3] << 24);
    double evm = SXD_SQRT(evm_sum / (evm_cnt > 0 ? evm_cnt : 1));
    out->valid = magic == MAGIC && out->packet[4] == 3 && sent == computed;
    if (!out->valid && repair_single_bit(out->packet, &sent, &computed)) {
        magic = MAGIC; out->valid = 1;
    }
    if (!out->valid && probe_whitening_index(scrambled, index, out->packet, &sent, &computed)) {
        magic = MAGIC; out->valid = 1;
    }
    out->sent = sent; out->computed = computed; out->score = score; out->evm = evm;
    out->snr = -20.0 * SXD_LOG10(evm > 1e-9 ? evm : 1e-9);
    out->phase = phase_sum / 8; out->timing = timing_sum / 8; out->swap = channel;
}

static double last_carrier_evm[CARRIERS];

static void decode_mono_robust(int at, uint32_t index, double score, sxd_result_t *best) {
    decode_mono_packet(at, index, score, mono_channel, best);
    if (best->valid) return;
    sxd_result_t cand;
    for (int delta = -16; delta <= 16; delta++) for (int ch = 0; ch < 2; ch++) {
        if (delta == 0 && ch == mono_channel) continue;
        int a2 = at + delta; if (a2 < 0 || a2 + MONO_PACKET_SAMPLES > audio_len) continue;
        decode_mono_packet(a2, index, score, ch, &cand);
        if (cand.valid) { *best = cand; return; }
        if (cand.evm < best->evm) *best = cand;
    }
}

static void decode_robust(int at, uint32_t index, double score, sxd_result_t *best) {
    decode_packet(at, index, score, 0, best);
    if (best->valid) return;
    sxd_result_t cand;
    /* feed-forward: the first attempt's pilot slope estimates the residual timing;
     * retry centred on it too (resampler / clock drift can exceed +/-4 samples) */
    int centres[2] = { 0, 0 }; int nc = 1;
    int t = (int)(best->timing >= 0 ? best->timing + 0.5 : best->timing - 0.5);
    if (t >= -24 && t <= 24 && t != 0) { centres[1] = t; nc = 2; }
    for (int ci = 0; ci < nc; ci++) {
        for (int delta = -4; delta <= 4; delta++) {
            for (int sw = 0; sw < 2; sw++) {
                if (ci == 0 && delta == 0 && sw == 0) continue;
                int a2 = at + centres[ci] + delta;
                if (a2 < 0) continue;
                decode_packet(a2, index, score, sw, &cand);
                if (cand.valid) { *best = cand; return; }
                if (cand.evm < best->evm) *best = cand;
            }
        }
    }
}

/* ================= public API ================= */

void sxd_reset(uint32_t start_index) {
    audio_len = 0; rs_len = 0; rs_base = 0; rs_pos = RS_HALF;
    armed = 1; locked = 0; carrier_reported = 0;
    expected_packet = start_index;
    received_samples = report_at = drops = 0;
    crc_ok = crc_bad = 0;
    if (sync_tpl[0] == 0.0 && sync_tpl[SYMBOL / 2] == 0.0) build_sync();
}

void sxd_set_mode(int mono, int channel) {
    mono_mode = mono != 0;
    mono_channel = channel != 0;
}

/* one packet slot exposed to JS/host */
typedef struct {
    uint8_t data[PACKET_BYTES];
    int32_t valid;
    uint32_t index, group, total_size, offset, image_crc;
    uint32_t sent, computed;
    int32_t shard, data_shards, payload_size, swap;
    float score, evm, snr, timing;
} sxd_packet_out_t;

/* decode as many packets as the buffer currently holds; returns count written */
int sxd_push(const float *left, const float *right, int frames, double rate,
             sxd_packet_out_t *out, int out_cap) {
    if (!armed) return 0;
    int before = audio_len;
    resample_push(left, right, frames, rate);
    received_samples += audio_len - before;

    if (!carrier_reported && audio_len >= 512) {
        double cre = 0, cim = 0, cpow = 0, cstep = 2.0 * SXD_PI * 6000.0 / RATE;
        int start = audio_len - 512;
        for (int i = 0; i < 512; i++) {
            double v = audioL[start + i];
            cre += v * SXD_COS(cstep * i); cim -= v * SXD_SIN(cstep * i); cpow += v * v;
        }
        double coh = (cre * cre + cim * cim) / (cpow * 512 > 1e-12 ? cpow * 512 : 1e-12);
        if (coh > 0.22) carrier_reported = 1;
    }

    int written = 0;
    for (;;) {
        double sc;
        int at = find_sync(&sc);
        if (at < 0) break;
        int packet_samples = mono_mode ? MONO_PACKET_SAMPLES : PACKET_SAMPLES;
        int packet_stride = ((packet_samples + 27) / 28) * 28;
        if (audio_len < at + packet_samples + 4) break;
        if (written >= out_cap) break;

        sxd_result_t r;
        if (mono_mode) decode_mono_robust(at, expected_packet, sc, &r);
        else decode_robust(at, expected_packet, sc, &r);
        /* If capture started after packet zero, pilot correlation still locks
         * but de-whitening with index zero makes every CRC fail.  Probe the
         * four indices FEC can cover, then continue from the recovered index. */
        if (!r.valid) {
            for (uint32_t skip = 1; skip <= FEC_PARITY_SHARDS; skip++) {
                sxd_result_t candidate;
                if (mono_mode) decode_mono_robust(at, expected_packet + skip, sc, &candidate);
                else decode_robust(at, expected_packet + skip, sc, &candidate);
                if (candidate.valid) { r = candidate; expected_packet += skip; break; }
            }
        }
        /* OFDM-only V6 has no external header to announce a transmitter
         * restart. If the console restarts at packet zero while the browser
         * still expects a high index, probe the first five whitening indices
         * and rebase automatically on a complete packet CRC match. */
        if (!r.valid && expected_packet > FEC_PARITY_SHARDS) {
            for (uint32_t restart = 0; restart <= FEC_PARITY_SHARDS; restart++) {
                sxd_result_t candidate;
                if (mono_mode) decode_mono_robust(at, restart, sc, &candidate);
                else decode_robust(at, restart, sc, &candidate);
                if (candidate.valid) { r = candidate; expected_packet = restart; break; }
            }
        }
        locked = sc >= 0.6;

        /* Before the first valid packet, a weak match can be the tail of the
         * FSK header rather than a lost OFDM packet.  Advance only a small
         * search step so packet zero and its whitening sequence stay aligned. */
        if (!r.valid && sc < 0.6) {
            int consume = at + 64;
            for (int i = 0; i + consume < audio_len; i++) { audioL[i] = audioL[i + consume]; audioR[i] = audioR[i + consume]; }
            audio_len -= consume;
            locked = 0;
            continue;
        }

        sxd_packet_out_t *o = &out[written++];
        for (int c = 0; c < CARRIERS; c++)
            last_carrier_evm[c] = r.carrier_count[c] ? SXD_SQRT(r.carrier_error[c] / r.carrier_count[c]) : -1.0;
        for (int i = 0; i < PACKET_BYTES; i++) o->data[i] = r.packet[i];
        o->valid = r.valid;
        o->sent = r.sent; o->computed = r.computed;
        o->score = (float)r.score; o->evm = (float)r.evm; o->snr = (float)r.snr;
        o->timing = (float)r.timing; o->swap = r.swap;
        if (r.valid) {
            crc_ok++;
            const uint8_t *p = r.packet;
            o->index = (uint32_t)p[22] | ((uint32_t)p[23] << 8);
            o->group = (uint32_t)p[6] | ((uint32_t)p[7] << 8);
            o->shard = p[8];
            o->data_shards = p[9];
            o->total_size = (uint32_t)p[12] | ((uint32_t)p[13] << 8) | ((uint32_t)p[14] << 16) | ((uint32_t)p[15] << 24);
            o->offset = (uint32_t)p[16] | ((uint32_t)p[17] << 8) | ((uint32_t)p[18] << 16) | ((uint32_t)p[19] << 24);
            o->payload_size = (uint32_t)p[20] | ((uint32_t)p[21] << 8);
            o->image_crc = (uint32_t)p[24] | ((uint32_t)p[25] << 8) | ((uint32_t)p[26] << 16) | ((uint32_t)p[27] << 24);
            expected_packet = o->index;
        } else {
            crc_bad++;
            o->index = expected_packet; o->group = 0; o->shard = 0; o->data_shards = 0;
            o->total_size = 0; o->offset = 0; o->payload_size = 0; o->image_crc = 0;
        }

        /* The SPU plays complete 28-sample ADPCM blocks.  Consume the same
         * padded stride as the transmitter (mono 5208, stereo 2884), rather
         * than leaving padding for the next sync search. */
        int consume_samples = packet_samples;
        if (packet_stride > packet_samples && audio_len >= at + packet_stride + 4) {
            double padding_peak = 0;
            for (int i = packet_samples; i < packet_stride; i++) {
                double l = audioL[at + i] < 0 ? -audioL[at + i] : audioL[at + i];
                double rr = audioR[at + i] < 0 ? -audioR[at + i] : audioR[at + i];
                if (l > padding_peak) padding_peak = l;
                if (rr > padding_peak) padding_peak = rr;
            }
            if (padding_peak < 0.02) consume_samples = packet_stride;
        }
        int consume = at + consume_samples;
        for (int i = 0; i + consume < audio_len; i++) { audioL[i] = audioL[i + consume]; audioR[i] = audioR[i + consume]; }
        audio_len -= consume;
        expected_packet++;
    }
    return written;
}

int sxd_carrier_detected(void) { return carrier_reported; }
int sxd_crc_ok(void) { return crc_ok; }
int sxd_crc_bad(void) { return crc_bad; }
long sxd_drops(void) { return drops; }
long sxd_received(void) { return received_samples; }
int sxd_buffered(void) { return audio_len; }
double sxd_carrier_evm(int carrier) {
    return carrier >= 0 && carrier < CARRIERS ? last_carrier_evm[carrier] : -1.0;
}

#ifndef SXD_HOST_TEST
/* ---- WASM entry points: fixed static buffers the JS worker reads/writes ---- */
#define SXD_IN_MAX 16384
#define SXD_OUT_MAX 64
static float g_in_l[SXD_IN_MAX], g_in_r[SXD_IN_MAX];
static sxd_packet_out_t g_out[SXD_OUT_MAX];
float *sxd_in_l(void) { return g_in_l; }
float *sxd_in_r(void) { return g_in_r; }
void *sxd_out(void) { return g_out; }
int sxd_in_capacity(void) { return SXD_IN_MAX; }
int sxd_out_capacity(void) { return SXD_OUT_MAX; }
int sxd_out_stride(void) { return (int)sizeof(sxd_packet_out_t); }
/* copy `frames` samples already written into g_in_l/g_in_r, decode, return count */
int sxd_process(int frames, double rate) {
    if (frames > SXD_IN_MAX) frames = SXD_IN_MAX;
    return sxd_push(g_in_l, g_in_r, frames, rate, g_out, SXD_OUT_MAX);
}
#endif
