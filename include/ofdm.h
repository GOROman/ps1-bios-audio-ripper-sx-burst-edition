#pragma once
#include <stddef.h>
#include <stdint.h>

#define SX_OFDM_MAGIC 0x314f5853u /* "SXO1" */
#define SX_OFDM_VERSION 3u
#define SX_OFDM_PACKET_BYTES 176u
#define SX_OFDM_FFT_SIZE 512u
#define SX_OFDM_CP_SIZE 64u
#define SX_OFDM_SYMBOLS_PACKET 5u
#define SX_OFDM_FIRST_BIN 24u
#define SX_OFDM_CARRIERS 96u
#define SX_OFDM_DATA_CARRIERS 88u
#define SX_OFDM_DATA_SHARDS 32u
#define SX_OFDM_PARITY_SHARDS 4u
#define SX_OFDM_TOTAL_SHARDS (SX_OFDM_DATA_SHARDS + SX_OFDM_PARITY_SHARDS)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint8_t version, flags;
    uint16_t group;
    uint8_t shard, data_shards, parity_shards, reserved;
    uint32_t total_size, offset;
    uint16_t payload_size, packet_index;
    uint32_t image_crc32, packet_crc32;
} sx_ofdm_header_t;

#define SX_OFDM_PAYLOAD_BYTES (SX_OFDM_PACKET_BYTES - sizeof(sx_ofdm_header_t))

size_t sx_ofdm_group_count(size_t size);
size_t sx_ofdm_packet_count(size_t size);
int sx_ofdm_make_packet(const uint8_t *data, size_t size, uint32_t image_crc,
                        size_t packet_index, uint8_t out[SX_OFDM_PACKET_BYTES]);
