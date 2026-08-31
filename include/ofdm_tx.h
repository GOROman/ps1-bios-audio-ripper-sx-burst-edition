#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SX_OFDM_TX_IDLE,
    SX_OFDM_TX_BUFFERING,
    SX_OFDM_TX_PLAYING,
    SX_OFDM_TX_DRAINING,
    SX_OFDM_TX_DONE,
    SX_OFDM_TX_ERROR
} sx_ofdm_tx_phase_t;

typedef struct {
    sx_ofdm_tx_phase_t phase;
    uint16_t generated_packets;
    uint16_t played_packet;
    uint16_t total_packets;
    uint8_t fifo_packets;
    uint8_t fifo_capacity;
    uint8_t last_generate_frames;
    uint8_t max_generate_frames;
    uint8_t packet_frames;
    uint8_t modem_frames;
    uint8_t adpcm_frames;
    uint16_t progress_permille;
    int error;
} sx_ofdm_tx_status_t;

int sx_ofdm_tx_begin(const uint8_t *data, size_t size);
void sx_ofdm_tx_release(void);
void sx_ofdm_tx_set_audio_mode(unsigned mode);
void sx_ofdm_tx_update(void);
void sx_ofdm_tx_stop(void);
void sx_ofdm_tx_play_fanfare(void);
const sx_ofdm_tx_status_t *sx_ofdm_tx_status(void);
