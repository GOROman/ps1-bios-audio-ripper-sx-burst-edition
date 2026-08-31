#include <stdint.h>
#include <string.h>
#include "audio_tx.h"
#include "ofdm_tx.h"
#include "sx_format.h"

/* Burst wire V6 is one continuous stereo OFDM stream.  The 32 KiB setting is
 * only a receiver/UI progress interval; no header or silence is inserted. */
static sx_tx_status_t status = { .phase = SX_TX_IDLE };
static unsigned logical_block_bytes = SX_WIRE_BLOCK_DEFAULT_BYTES;

const sx_tx_status_t *sx_audio_status(void) { return &status; }

void sx_audio_set_mode(unsigned mode) {
    (void)mode;
    sx_ofdm_tx_set_audio_mode(0u);
}

void sx_audio_set_block_bytes(unsigned bytes) {
    if (bytes >= 1024u && bytes <= SX_WIRE_BLOCK_MAX_BYTES && !(bytes & (bytes - 1u)))
        logical_block_bytes = bytes;
}

int sx_audio_transmit(const uint8_t *container, size_t size) {
    if (!container || size < sizeof(sx_header_t) ||
        ((const sx_header_t *)container)->magic != SX_MAGIC) return -1;

    sx_ofdm_tx_stop();
    memset(&status, 0, sizeof(status));
    status.phase = SX_TX_PREPARING;
    status.blocks = (uint32_t)((size + logical_block_bytes - 1u) / logical_block_bytes);
    status.block_bytes = logical_block_bytes;
    status.block_crc = sx_crc32(container, size, 0);
    if (sx_ofdm_tx_begin(container, size) < 0) {
        status.phase = SX_TX_ERROR;
        status.error = -3;
        return -1;
    }
    /* Start automatically after the safe OFDM FIFO depth is ready. */
    sx_ofdm_tx_release();
    return 0;
}

int sx_audio_transmit_block(const uint8_t *container, size_t size, unsigned sequence) {
    (void)container;
    (void)size;
    (void)sequence;
    /* Legacy one-shot blocks are not part of the continuous Burst transport. */
    status.phase = SX_TX_ERROR;
    status.error = -11;
    return -1;
}

void sx_audio_stop(void) {
    sx_ofdm_tx_stop();
    status.phase = SX_TX_IDLE;
    status.current_samples = 0;
    status.total_samples = 0;
    status.progress_permille = 0;
    status.error = 0;
}

void sx_audio_update(void) {
    if (status.phase != SX_TX_PREPARING && status.phase != SX_TX_SENDING) return;
    sx_ofdm_tx_update();
    const sx_ofdm_tx_status_t *ofdm = sx_ofdm_tx_status();
    status.progress_permille = ofdm->progress_permille;
    status.block = status.blocks ?
        (uint32_t)(((uint64_t)status.progress_permille * status.blocks) / 1000u) : 0;
    if (status.block > status.blocks) status.block = status.blocks;

    if (ofdm->phase == SX_OFDM_TX_BUFFERING) status.phase = SX_TX_PREPARING;
    else if (ofdm->phase == SX_OFDM_TX_PLAYING || ofdm->phase == SX_OFDM_TX_DRAINING)
        status.phase = SX_TX_SENDING;
    else if (ofdm->phase == SX_OFDM_TX_DONE) {
        status.block = status.blocks;
        status.progress_permille = 1000;
        status.phase = SX_TX_DONE;
    } else if (ofdm->phase == SX_OFDM_TX_ERROR) {
        status.phase = SX_TX_ERROR;
        status.error = ofdm->error;
    }
}
