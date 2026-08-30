#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define SXD_HOST_TEST 1
#include "../src/ofdm_demod.c"

static uint16_t u16(const uint8_t *p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t u32(const uint8_t *p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }

int main(int argc, char **argv) {
    bool mono = false; int channel = 0; uint32_t start = 0; const char *path = nullptr;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--mono")) mono = true;
        else if (!std::strcmp(argv[i], "--channel") && i + 1 < argc) channel = std::atoi(argv[++i]) - 1;
        else if (!std::strcmp(argv[i], "--start") && i + 1 < argc) start = uint32_t(std::strtoul(argv[++i], nullptr, 0));
        else path = argv[i];
    }
    if (!path || channel < 0) {
        std::fprintf(stderr, "usage: %s [--mono] [--channel 1|2] [--start N] capture.wav\n", argv[0]);
        return 2;
    }
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::perror(path); return 2; }
    uint8_t head[12];
    if (std::fread(head, 1, 12, f) != 12 || std::memcmp(head, "RIFF", 4) || std::memcmp(head + 8, "WAVE", 4)) {
        std::fprintf(stderr, "not a RIFF/WAVE file\n"); return 2;
    }
    int channels = 0, bits = 0, format = 0; uint32_t rate = 0; long data_at = -1; uint32_t data_size = 0;
    uint8_t ch[8];
    while (std::fread(ch, 1, 8, f) == 8) {
        uint32_t size = u32(ch + 4); long payload = std::ftell(f);
        if (!std::memcmp(ch, "fmt ", 4)) {
            std::vector<uint8_t> fmt(size); if (std::fread(fmt.data(), 1, size, f) != size) return 2;
            format = u16(fmt.data()); channels = u16(fmt.data() + 2); rate = u32(fmt.data() + 4); bits = u16(fmt.data() + 14);
            if (format == 0xfffe && size >= 26) format = u16(fmt.data() + 24);
        } else if (!std::memcmp(ch, "data", 4)) { data_at = payload; data_size = size; std::fseek(f, size, SEEK_CUR); }
        else std::fseek(f, size, SEEK_CUR);
        if (size & 1) std::fseek(f, 1, SEEK_CUR);
    }
    if (format != 1 || bits != 16 || channels < 1 || channel >= channels || data_at < 0) {
        std::fprintf(stderr, "unsupported WAV: format=%d bits=%d channels=%d\n", format, bits, channels); return 2;
    }
    const size_t frames = data_size / (channels * sizeof(int16_t));
    std::vector<int16_t> pcm(frames * channels); std::fseek(f, data_at, SEEK_SET);
    if (std::fread(pcm.data(), sizeof(int16_t), pcm.size(), f) != pcm.size()) return 2;
    std::fclose(f);
    std::printf("WAV %u Hz, %d ch, %zu frames; mode=%s input=ch%d start=%u\n", rate, channels, frames, mono ? "mono" : "stereo", channel + 1, start);

    sxd_set_mode(mono, channel); sxd_reset(start);
    int seen = 0, ok = 0;
    double carrier_sum[CARRIERS] = {}; int carrier_n[CARRIERS] = {};
    for (size_t pos = 0; pos < frames; pos += 2048) {
        int n = int((frames - pos) < 2048 ? frames - pos : 2048);
        std::vector<float> l(n), r(n);
        for (int i = 0; i < n; i++) {
            l[i] = pcm[(pos + i) * channels + channel] / 32768.0f;
            int rc = mono ? channel : (channel + 1 < channels ? channel + 1 : channel);
            r[i] = pcm[(pos + i) * channels + rc] / 32768.0f;
        }
        sxd_packet_out_t out[16]; int got = sxd_push(l.data(), r.data(), n, rate, out, 16);
        for (int i = 0; i < got; i++) {
            seen++; ok += out[i].valid != 0;
            std::printf("PKT %4u  %s  C=%.3f SNR=%5.1f EVM=%5.1f%% T=%6.2f CRC=%08x/%08x",
                        out[i].index, out[i].valid ? "OK " : "BAD", out[i].score, out[i].snr,
                        out[i].evm * 100.0f, out[i].timing, out[i].sent, out[i].computed);
            if (out[i].valid) std::printf(" G=%u S=%d SIZE=%u", out[i].group, out[i].shard, out[i].payload_size);
            std::putchar('\n');
            if (mono) for (int c = 0; c < CARRIERS; c++) {
                double e = sxd_carrier_evm(c); if (e >= 0) { carrier_sum[c] += e; carrier_n[c]++; }
            }
        }
    }
    std::printf("RESULT packets=%d crc_ok=%d crc_bad=%d drops=%ld carrier=%s\n",
                seen, ok, seen - ok, sxd_drops(), sxd_carrier_detected() ? "yes" : "no");
    if (mono && seen) {
        std::puts("CARRIER EVM (8-carrier bands):");
        for (int first = 0; first < CARRIERS; first += 8) {
            double sum = 0; int n = 0;
            for (int c = first; c < first + 8; c++) if (carrier_n[c]) { sum += carrier_sum[c] / carrier_n[c]; n++; }
            double hz0 = double(FIRST_BIN + first) * RATE / FFT;
            double hz1 = double(FIRST_BIN + first + 7) * RATE / FFT;
            std::printf("  C%02d-%02d %5.0f-%5.0f Hz  %5.1f%%\n", first, first + 7, hz0, hz1, n ? sum / n * 100 : 0);
        }
        struct Worst { int carrier; double evm; } worst[8];
        for (auto &w : worst) w = {-1, -1};
        for (int c = 0; c < CARRIERS; c++) if (carrier_n[c]) {
            double e = carrier_sum[c] / carrier_n[c];
            for (int i = 0; i < 8; i++) if (e > worst[i].evm) {
                for (int j = 7; j > i; j--) worst[j] = worst[j - 1];
                worst[i] = {c, e}; break;
            }
        }
        std::puts("WORST CARRIERS:");
        for (const auto &w : worst) if (w.carrier >= 0)
            std::printf("  C%02d bin=%3d %7.1f Hz  EVM=%5.1f%%\n", w.carrier,
                        FIRST_BIN + w.carrier, double(FIRST_BIN + w.carrier) * RATE / FFT, w.evm * 100);
    }
    return ok ? 0 : 1;
}
