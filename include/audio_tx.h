#pragma once

#include <stddef.h>
#include <stdint.h>

#define SX_SAMPLE_RATE            44100u
#define SX_WIRE_BLOCK_DEFAULT_BYTES 32768u
#define SX_WIRE_BLOCK_MAX_BYTES  131072u
#define SX_WIRE_VERSION           6u

typedef enum {
    SX_TX_IDLE, SX_TX_PREPARING, SX_TX_READY, SX_TX_CALIBRATION,
    SX_TX_HEADER, SX_TX_SENDING, SX_TX_DONE, SX_TX_ERROR
} sx_tx_phase_t;
typedef struct {
    sx_tx_phase_t phase;
    uint32_t block, blocks;
    uint32_t block_crc, block_bytes;
    uint32_t current_samples, total_samples;
    uint16_t progress_permille, header_bytes;
    int error;
} sx_tx_status_t;

const sx_tx_status_t *sx_audio_status(void);
void sx_audio_set_mode(unsigned mode);
void sx_audio_set_block_bytes(unsigned bytes);
int sx_audio_transmit(const uint8_t *container, size_t size);
int sx_audio_transmit_block(const uint8_t *container, size_t size, unsigned sequence);
void sx_audio_stop(void);
void sx_audio_update(void);
