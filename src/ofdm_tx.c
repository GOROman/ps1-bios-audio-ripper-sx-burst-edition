#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <psxapi.h>
#include <psxetc.h>
#include <psxgpu.h>
#include <psxspu.h>
#include <hwregs_c.h>
#include "audio_tx.h"
#include "ofdm.h"
#include "ofdm_mod.h"
#include "ofdm_tx.h"
#include "spu_adpcm.h"
#include "sx_format.h"

/* Keep OFDM in a dedicated SPU RAM region so a previous modem voice cannot
 * leak into the left channel while the following DMA update settles. */
#define SPU_BUFFER_ADDR 0x20000u
#define MAX_ADPCM_BLOCKS ((SX_OFDM_MAX_PACKET_SAMPLES + SX_SPU_ADPCM_SAMPLES_PER_BLOCK - 1u) / SX_SPU_ADPCM_SAMPLES_PER_BLOCK)
#define MAX_CHANNEL_BYTES (MAX_ADPCM_BLOCKS * SX_SPU_ADPCM_BLOCK_BYTES)
#define MAX_CHUNK_BYTES (MAX_CHANNEL_BYTES * 2u)
#define FIFO_CAPACITY 8u
#define START_PACKETS FIFO_CAPACITY
#define PRECOMPUTE_PACKETS (SX_OFDM_TOTAL_SHARDS * 2u)
#define START_SIGNAL_ADDR 0x68000u
#define START_BEEP_SHORT_SAMPLES 19404u /* 440 ms */
#define START_BEEP_GAP_SAMPLES  15876u /* 360 ms */
#define START_BEEP_LONG_SAMPLES 61740u /* 1400 ms */
#define START_BEEP_TAIL_SAMPLES  8820u /* 200 ms */
#define START_SIGNAL_SAMPLES (START_BEEP_SHORT_SAMPLES * 2u + START_BEEP_GAP_SAMPLES * 2u + START_BEEP_LONG_SAMPLES + START_BEEP_TAIL_SAMPLES)
#define START_SIGNAL_BLOCKS ((START_SIGNAL_SAMPLES + SX_SPU_ADPCM_SAMPLES_PER_BLOCK - 1u) / SX_SPU_ADPCM_SAMPLES_PER_BLOCK)
#define START_SIGNAL_BYTES (START_SIGNAL_BLOCKS * SX_SPU_ADPCM_BLOCK_BYTES)
#define FANFARE_ADDR 0x20000u

static sx_ofdm_tx_status_t status;
static const uint8_t *source;
static size_t source_size, packet_count, next_packet;
static uint32_t image_crc;
static uint8_t (*fifo)[MAX_CHUNK_BYTES];
static uint8_t (*precomputed)[PRECOMPUTE_PACKETS * MAX_CHANNEL_BYTES];
static uint16_t fifo_packet[FIFO_CAPACITY];
static volatile uint8_t fifo_head, fifo_tail, fifo_count;
static volatile uint8_t db_active, dma_busy, stream_active, terminal_generated;
static volatile uint8_t playback_hold, callbacks_installed;
static volatile int drain_vsync;
static int refresh_rate, precompute_mode, precompute_start_vsync;
static size_t packet_samples, playback_packet_samples, adpcm_blocks, channel_bytes, chunk_bytes;
static uint8_t *packet;
static uint8_t *start_signal_adpcm;
static uint8_t start_signal_ready, start_signal_playing;
static int start_signal_vsync;
extern const uint8_t fanfare_adpcm[];
extern const size_t fanfare_adpcm_size;
static uint8_t fanfare_ready, fanfare_playing;
static unsigned audio_mode;
static int16_t (*pcm)[SX_OFDM_MAX_PACKET_SAMPLES];
static void *old_spu_callback, *old_dma_callback;

static void release_work_buffers(void) {
    free(fifo); fifo = NULL;
    free(precomputed); precomputed = NULL;
    free(packet); packet = NULL;
    free(start_signal_adpcm); start_signal_adpcm = NULL;
    free(pcm); pcm = NULL;
    start_signal_ready = 0;
}

static int allocate_work_buffers(int use_precomputed) {
    if (!packet) packet = malloc(SX_OFDM_PACKET_BYTES);
    if (!pcm) pcm = malloc(sizeof(*pcm) * 2u);
    if (!start_signal_adpcm) start_signal_adpcm = malloc(START_SIGNAL_BYTES);
    if (use_precomputed) {
        if (!precomputed) precomputed = malloc(sizeof(*precomputed) * 2u);
    } else {
        if (!fifo) fifo = malloc(sizeof(*fifo) * FIFO_CAPACITY);
    }
    if (!packet || !pcm || !start_signal_adpcm ||
        (use_precomputed ? !precomputed : !fifo)) {
        release_work_buffers();
        return 0;
    }
    return 1;
}

static void stop_channels(void) {
    /* KeyOff follows the ADSR release and is not instant on every real SPU.
     * Zero all modem voice volumes before starting the next OFDM stream. */
    SPU_CH_VOL_L(0) = SPU_CH_VOL_R(0) = 0;
    SPU_CH_VOL_L(1) = SPU_CH_VOL_R(1) = 0;
    SPU_CH_VOL_L(2) = SPU_CH_VOL_R(2) = 0;
    SPU_CH_VOL_L(3) = SPU_CH_VOL_R(3) = 0;
    SpuSetKey(0, 0x0f);
    SPU_CTRL &= ~(1 << 6);
}

static int16_t triangle_tone(unsigned sample, unsigned length, unsigned frequency) {
    uint32_t phase = (uint32_t)(((uint64_t)sample * frequency * 65536u) / SX_SAMPLE_RATE) & 0xffffu;
    int32_t triangle;
    if (phase < 16384u) triangle = (int32_t)phase * 2;
    else if (phase < 49152u) triangle = 32768 - (int32_t)(phase - 16384u) * 2;
    else triangle = -32768 + (int32_t)(phase - 49152u) * 2;
    unsigned fade = 220u;
    unsigned gain = sample < fade ? sample : (length - 1u - sample < fade ? length - 1u - sample : fade);
    return (int16_t)(((int64_t)triangle * 15000 * (int32_t)gain) /
                     (32768 * (int32_t)fade));
}

static int16_t start_signal_sample(unsigned sample) {
    if (sample < START_BEEP_SHORT_SAMPLES)
        return triangle_tone(sample, START_BEEP_SHORT_SAMPLES, 1800u);
    sample -= START_BEEP_SHORT_SAMPLES;
    if (sample < START_BEEP_GAP_SAMPLES) return 0;
    sample -= START_BEEP_GAP_SAMPLES;
    if (sample < START_BEEP_SHORT_SAMPLES)
        return triangle_tone(sample, START_BEEP_SHORT_SAMPLES, 1800u);
    sample -= START_BEEP_SHORT_SAMPLES;
    if (sample < START_BEEP_GAP_SAMPLES) return 0;
    sample -= START_BEEP_GAP_SAMPLES;
    if (sample < START_BEEP_LONG_SAMPLES)
        return triangle_tone(sample, START_BEEP_LONG_SAMPLES, 900u);
    return 0;
}

static void prepare_start_signal(void) {
    if (start_signal_ready) return;
    sx_spu_adpcm_state_t state;
    sx_spu_adpcm_reset(&state);
    for (unsigned block = 0; block < START_SIGNAL_BLOCKS; block++) {
        int16_t samples[SX_SPU_ADPCM_SAMPLES_PER_BLOCK];
        for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
            unsigned at = block * SX_SPU_ADPCM_SAMPLES_PER_BLOCK + i;
            samples[i] = at < START_SIGNAL_SAMPLES ? start_signal_sample(at) : 0;
        }
        sx_spu_adpcm_encode_block_fast(&state, samples,
            block + 1u == START_SIGNAL_BLOCKS ? 1u : 0u,
            start_signal_adpcm + block * SX_SPU_ADPCM_BLOCK_BYTES);
    }
    start_signal_ready = 1;
}

static int prepare_fanfare(void) {
    fanfare_ready = 1;
    return 1;
}

static void start_signal(void) {
    prepare_start_signal();
    stop_channels();
    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    SpuSetTransferStartAddr(START_SIGNAL_ADDR);
    SpuWrite((const uint32_t *)start_signal_adpcm, START_SIGNAL_BYTES);
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
    SPU_CH_ADDR(2) = SPU_CH_ADDR(3) = getSPUAddr(START_SIGNAL_ADDR);
    SPU_CH_FREQ(2) = SPU_CH_FREQ(3) = getSPUSampleRate(SX_SAMPLE_RATE);
    SPU_CH_ADSR1(2) = SPU_CH_ADSR1(3) = 0x00ff;
    SPU_CH_ADSR2(2) = SPU_CH_ADSR2(3) = 0;
    SPU_CH_VOL_L(2) = 0x2fff; SPU_CH_VOL_R(2) = 0;
    SPU_CH_VOL_L(3) = 0; SPU_CH_VOL_R(3) = 0x2fff;
    SpuSetKey(1, 0x0c);
    start_signal_vsync = VSync(-1);
    start_signal_playing = 1;
}

void sx_ofdm_tx_play_fanfare(void) {
    if (fanfare_playing) return;
    if (!prepare_fanfare()) return;
    stop_channels();
    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    SpuSetTransferStartAddr(FANFARE_ADDR);
    SpuWrite((const uint32_t *)fanfare_adpcm, fanfare_adpcm_size);
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
    SPU_CH_ADDR(2) = SPU_CH_ADDR(3) = getSPUAddr(FANFARE_ADDR);
    SPU_CH_FREQ(2) = SPU_CH_FREQ(3) = getSPUSampleRate(SX_SAMPLE_RATE);
    SPU_CH_ADSR1(2) = SPU_CH_ADSR1(3) = 0x00ff;
    SPU_CH_ADSR2(2) = SPU_CH_ADSR2(3) = 0;
    SPU_CH_VOL_L(2) = 0x2fff; SPU_CH_VOL_R(2) = 0;
    SPU_CH_VOL_L(3) = 0; SPU_CH_VOL_R(3) = 0x2fff;
    SpuSetKey(1, 0x0c);
    fanfare_playing = 1;
}

static void spu_dma_handler(void) {
    dma_busy = 0;
    if (stream_active) SPU_CTRL |= 1 << 6;
}

static void spu_irq_handler(void) {
    SPU_CTRL &= ~(1 << 6);
    if (!stream_active) return;
    if (!fifo_count) {
        if (terminal_generated) {
            status.phase = SX_OFDM_TX_DRAINING;
            drain_vsync = VSync(-1);
        } else {
            status.phase = SX_OFDM_TX_ERROR;
            status.error = -2;
            stop_channels();
        }
        return;
    }

    uint8_t slot = fifo_tail;
    fifo_tail = (uint8_t)((fifo_tail + 1u) % FIFO_CAPACITY);
    fifo_count--;
    db_active ^= 1;
    uint32_t address = SPU_BUFFER_ADDR + (db_active ? MAX_CHUNK_BYTES : 0u);
    status.played_packet = fifo_packet[slot];
    status.fifo_packets = fifo_count;
    SPU_IRQ_ADDR = getSPUAddr(address);
    SPU_CH_LOOP_ADDR(0) = getSPUAddr(address);
    SPU_CH_LOOP_ADDR(1) = getSPUAddr(address + channel_bytes);
    SpuSetTransferStartAddr(address);
    dma_busy = 1;
    SpuWrite((const uint32_t *)fifo[slot], chunk_bytes);
}

static void encode_channel(unsigned channel, uint8_t *output, uint8_t final_flags) {
    sx_spu_adpcm_state_t state;
    sx_spu_adpcm_reset(&state);
    for (unsigned block = 0; block < adpcm_blocks; block++) {
        int16_t padded[SX_SPU_ADPCM_SAMPLES_PER_BLOCK] = { 0 };
        unsigned source_channel = audio_mode == 1u ? (channel ^ 1u) : channel;
        for (unsigned i = 0; i < SX_SPU_ADPCM_SAMPLES_PER_BLOCK; i++) {
            unsigned index = block * SX_SPU_ADPCM_SAMPLES_PER_BLOCK + i;
            if (index < packet_samples) padded[i] = pcm[source_channel][index];
        }
        sx_spu_adpcm_encode_block_fast(&state, padded,
            block + 1u == adpcm_blocks ? final_flags : 0,
            output + block * SX_SPU_ADPCM_BLOCK_BYTES);
    }
}

void sx_ofdm_tx_set_audio_mode(unsigned mode) { (void)mode; audio_mode = 0u; }

static int generate_precomputed(void) {
    int stage = VSync(-1);
    if (!sx_ofdm_make_packet(source, source_size, image_crc, next_packet, packet)) return 0;
    int now = VSync(-1); status.packet_frames = (uint8_t)(now - stage); stage = now;
    if (audio_mode == 2u) sx_ofdm_modulate_packet_mono(packet, (uint32_t)next_packet, pcm[0], pcm[1]);
    else sx_ofdm_modulate_packet(packet, (uint32_t)next_packet, pcm[0], pcm[1]);
    now = VSync(-1); status.modem_frames = (uint8_t)(now - stage); stage = now;
    uint8_t final = next_packet + 1u == packet_count ? 1u : 0u;
    encode_channel(0, precomputed[0] + next_packet * channel_bytes, final);
    encode_channel(1, precomputed[1] + next_packet * channel_bytes, final);
    now = VSync(-1); status.adpcm_frames = (uint8_t)(now - stage);
    next_packet++; status.generated_packets = (uint16_t)next_packet;
    status.fifo_packets = (uint8_t)next_packet;
    return 1;
}

static void start_precomputed(void) {
    size_t stream_channel_bytes = packet_count * channel_bytes;
    uint32_t right_address = SPU_BUFFER_ADDR + (uint32_t)stream_channel_bytes;
    stop_channels();
    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    /* A 4 KiB stereo wire block is 40 * 1648 = 65920 bytes per
     * channel, just over 64 KiB.  Large single DMA writes work in
     * emulators but are unreliable on real SPUs, so upload one modem
     * packet at a time while retaining contiguous playback in SPU RAM. */
    for (size_t i = 0; i < packet_count; i++) {
        uint32_t offset = (uint32_t)(i * channel_bytes);
        SpuSetTransferStartAddr(SPU_BUFFER_ADDR + offset);
        SpuWrite((const uint32_t *)(precomputed[0] + offset), channel_bytes);
        SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
        SpuSetTransferStartAddr(right_address + offset);
        SpuWrite((const uint32_t *)(precomputed[1] + offset), channel_bytes);
        SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
    }
    /* Keep the OFDM voices independent from any previous modem voice. Re-keying a voice
     * immediately after an ADPCM end block is not reliable on every SPU path. */
    SPU_CH_ADDR(2) = getSPUAddr(SPU_BUFFER_ADDR);
    SPU_CH_ADDR(3) = getSPUAddr(right_address);
    SPU_CH_FREQ(2) = SPU_CH_FREQ(3) = getSPUSampleRate(SX_SAMPLE_RATE);
    SPU_CH_ADSR1(2) = SPU_CH_ADSR1(3) = 0x00ff;
    SPU_CH_ADSR2(2) = SPU_CH_ADSR2(3) = 0;
    SPU_CH_VOL_L(2) = 0x2fff; SPU_CH_VOL_R(2) = 0;
    SPU_CH_VOL_L(3) = 0; SPU_CH_VOL_R(3) = 0x2fff;
    SpuSetKey(1, 0x0c);
    precompute_start_vsync = VSync(-1);
    status.phase = SX_OFDM_TX_PLAYING;
}

static int generate_chunk(uint8_t slot) {
    uint8_t *target = fifo[slot];
    if (next_packet < packet_count) {
        int stage = VSync(-1);
        if (!sx_ofdm_make_packet(source, source_size, image_crc, next_packet, packet)) return 0;
        int now = VSync(-1);
        status.packet_frames = (uint8_t)(now - stage);
        stage = now;
        if (audio_mode == 2u) sx_ofdm_modulate_packet_mono(packet, (uint32_t)next_packet, pcm[0], pcm[1]);
        else sx_ofdm_modulate_packet(packet, (uint32_t)next_packet, pcm[0], pcm[1]);
        now = VSync(-1);
        status.modem_frames = (uint8_t)(now - stage);
        stage = now;
        encode_channel(0, target, 3);
        encode_channel(1, target + channel_bytes, 3);
        now = VSync(-1);
        status.adpcm_frames = (uint8_t)(now - stage);
        fifo_packet[slot] = (uint16_t)next_packet;
        next_packet++;
        status.generated_packets = (uint16_t)next_packet;
    } else {
        memset(pcm, 0, sizeof(*pcm) * 2u);
        encode_channel(0, target, 1);
        encode_channel(1, target + channel_bytes, 1);
        fifo_packet[slot] = (uint16_t)packet_count;
        terminal_generated = 1;
    }
    return 1;
}

static void start_stream(void) {
    stream_active = 1;
    db_active = 0;
    /* Prime both SPU buffers before KeyOn.  Starting playback after only the
     * first upload leaves packet #1 racing its DMA on real hardware even
     * though emulators complete the transfer effectively immediately. */
    spu_irq_handler();
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
    uint32_t address = SPU_BUFFER_ADDR + (db_active ? MAX_CHUNK_BYTES : 0u);
    SPU_CTRL &= ~(1 << 6);
    SpuSetKey(0, 3);
    SPU_CH_ADDR(0) = getSPUAddr(address);
    SPU_CH_ADDR(1) = getSPUAddr(address + channel_bytes);
    SPU_CH_FREQ(0) = SPU_CH_FREQ(1) = getSPUSampleRate(SX_SAMPLE_RATE);
    SPU_CH_ADSR1(0) = SPU_CH_ADSR1(1) = 0x00ff;
    SPU_CH_ADSR2(0) = SPU_CH_ADSR2(1) = 0;
    SPU_CH_VOL_L(0) = 0x2fff; SPU_CH_VOL_R(0) = 0;
    SPU_CH_VOL_L(1) = 0; SPU_CH_VOL_R(1) = 0x2fff;
    spu_irq_handler();
    SpuIsTransferCompleted(SPU_TRANSFER_WAIT);
    SpuSetKey(1, 3);
    status.phase = SX_OFDM_TX_PLAYING;
}

int sx_ofdm_tx_begin(const uint8_t *data, size_t size) {
    if (!data || !size) return -1;
    memset(&status, 0, sizeof(status));
    source = data;
    source_size = size;
    packet_count = sx_ofdm_packet_count(size);
    if (!packet_count || packet_count > UINT16_MAX) return -1;
    image_crc = sx_crc32(data, size, 0);
    packet_samples = audio_mode == 2u ? SX_OFDM_MONO_PACKET_SAMPLES : SX_OFDM_PACKET_SAMPLES;
    adpcm_blocks = (packet_samples + SX_SPU_ADPCM_SAMPLES_PER_BLOCK - 1u) / SX_SPU_ADPCM_SAMPLES_PER_BLOCK;
    /* The SPU plays complete 28-sample ADPCM blocks, including padding after
     * the modem PCM. */
    playback_packet_samples = adpcm_blocks * SX_SPU_ADPCM_SAMPLES_PER_BLOCK;
    channel_bytes = adpcm_blocks * SX_SPU_ADPCM_BLOCK_BYTES;
    chunk_bytes = channel_bytes * 2u;
    next_packet = 0;
    fifo_head = fifo_tail = fifo_count = 0;
    terminal_generated = dma_busy = stream_active = start_signal_playing = 0;
    playback_hold = 1;
    refresh_rate = GetVideoMode() == MODE_PAL ? 50 : 60;
    precompute_mode = packet_count <= PRECOMPUTE_PACKETS;
    if (!allocate_work_buffers(precompute_mode)) {
        status.phase = SX_OFDM_TX_ERROR;
        status.error = -3;
        return -1;
    }
    status.phase = SX_OFDM_TX_BUFFERING;
    status.total_packets = (uint16_t)packet_count;
    status.fifo_capacity = (uint8_t)(precompute_mode ? packet_count : FIFO_CAPACITY);
    status.played_packet = 0;
    SpuSetTransferMode(SPU_TRANSFER_BY_DMA);
    if (precompute_mode) return 0;
    if (!callbacks_installed) {
        int restore = EnterCriticalSection();
        old_spu_callback = InterruptCallback(IRQ_SPU, &spu_irq_handler);
        old_dma_callback = DMACallback(DMA_SPU, &spu_dma_handler);
        if (restore) ExitCriticalSection();
        callbacks_installed = 1;
    }
    return 0;
}

void sx_ofdm_tx_release(void) { playback_hold = 0; }

void sx_ofdm_tx_update(void) {
    if (status.phase == SX_OFDM_TX_IDLE || status.phase == SX_OFDM_TX_DONE || status.phase == SX_OFDM_TX_ERROR) return;
    if (start_signal_playing) {
        int frames = VSync(-1) - start_signal_vsync;
        int needed = (int)((START_SIGNAL_SAMPLES * (unsigned)refresh_rate + SX_SAMPLE_RATE - 1u) / SX_SAMPLE_RATE) + 1;
        if (frames >= needed) {
            SpuSetKey(0, 0x0c);
            SPU_CH_VOL_L(2) = SPU_CH_VOL_R(2) = 0;
            SPU_CH_VOL_L(3) = SPU_CH_VOL_R(3) = 0;
            start_signal_playing = 0;
            if (precompute_mode) start_precomputed(); else start_stream();
        }
        return;
    }
    if (precompute_mode) {
        if (status.phase == SX_OFDM_TX_PLAYING) {
            int frames = VSync(-1) - precompute_start_vsync;
            uint32_t samples = (uint32_t)(((uint64_t)(unsigned)frames * SX_SAMPLE_RATE) / (unsigned)refresh_rate);
            unsigned played = samples / playback_packet_samples;
            if (played > packet_count) played = (unsigned)packet_count;
            status.played_packet = (uint16_t)played;
            status.progress_permille = (uint16_t)(((uint32_t)played * 1000u) / packet_count);
            uint32_t end_guard = (SX_SAMPLE_RATE + (unsigned)refresh_rate - 1u) /
                                 (unsigned)refresh_rate;
            if (samples >= packet_count * playback_packet_samples + end_guard) {
                stop_channels(); status.played_packet = status.total_packets;
                status.progress_permille = 1000; status.phase = SX_OFDM_TX_DONE;
            }
            return;
        }
        if (next_packet < packet_count) {
            int start = VSync(-1);
            if (!generate_precomputed()) { status.phase = SX_OFDM_TX_ERROR; status.error = -1; return; }
            int frames = VSync(-1) - start;
            if (frames < 0) frames = 0; if (frames > 255) frames = 255;
            status.last_generate_frames = (uint8_t)frames;
            if (frames > status.max_generate_frames) status.max_generate_frames = (uint8_t)frames;
        }
        if (!playback_hold && next_packet == packet_count) start_signal();
        return;
    }
    if (status.phase == SX_OFDM_TX_DRAINING) {
        int frames = VSync(-1) - drain_vsync;
        int needed = (int)((playback_packet_samples * (unsigned)refresh_rate + SX_SAMPLE_RATE - 1u) / SX_SAMPLE_RATE) + 1;
        if (frames >= needed) {
            stop_channels();
            stream_active = 0;
            status.played_packet = status.total_packets;
            status.progress_permille = 1000;
            status.phase = SX_OFDM_TX_DONE;
        }
        return;
    }

    if (!terminal_generated && fifo_count < FIFO_CAPACITY && !dma_busy) {
        uint8_t slot = fifo_head;
        int generate_start = VSync(-1);
        if (!generate_chunk(slot)) {
            status.phase = SX_OFDM_TX_ERROR;
            status.error = -1;
            stop_channels();
            return;
        }
        int generate_frames = VSync(-1) - generate_start;
        if (generate_frames < 0) generate_frames = 0;
        if (generate_frames > 255) generate_frames = 255;
        status.last_generate_frames = (uint8_t)generate_frames;
        if (generate_frames > status.max_generate_frames) status.max_generate_frames = (uint8_t)generate_frames;
        FastEnterCriticalSection();
        fifo_head = (uint8_t)((fifo_head + 1u) % FIFO_CAPACITY);
        fifo_count++;
        status.fifo_packets = fifo_count;
        FastExitCriticalSection();
    }
    if (!playback_hold && status.phase == SX_OFDM_TX_BUFFERING &&
        (fifo_count >= START_PACKETS || terminal_generated)) start_signal();
    if (status.total_packets) {
        unsigned played = status.played_packet;
        if (played > status.total_packets) played = status.total_packets;
        status.progress_permille = (uint16_t)(((uint32_t)played * 1000u) / status.total_packets);
    }
}

void sx_ofdm_tx_stop(void) {
    stop_channels();
    stream_active = start_signal_playing = 0;
    fanfare_playing = 0;
    if (callbacks_installed) {
        int restore = EnterCriticalSection();
        InterruptCallback(IRQ_SPU, old_spu_callback);
        DMACallback(DMA_SPU, old_dma_callback);
        if (restore) ExitCriticalSection();
        callbacks_installed = 0;
    }
    status.phase = SX_OFDM_TX_IDLE;
}

const sx_ofdm_tx_status_t *sx_ofdm_tx_status(void) { return &status; }
