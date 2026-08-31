#include <string.h>
#include "ofdm.h"
#include "sx_format.h"

static uint8_t gf_exp[512], gf_log[256];
static int gf_ready;

static void gf_init(void) {
    if (gf_ready) return;
    unsigned value = 1;
    for (unsigned i = 0; i < 255; i++) {
        gf_exp[i] = (uint8_t)value; gf_log[value] = (uint8_t)i;
        value <<= 1; if (value & 0x100) value ^= 0x11d;
    }
    for (unsigned i = 255; i < 512; i++) gf_exp[i] = gf_exp[i - 255];
    gf_ready = 1;
}

static uint8_t gf_mul(uint8_t a, uint8_t b) {
    return (!a || !b) ? 0 : gf_exp[(unsigned)gf_log[a] + gf_log[b]];
}

size_t sx_ofdm_group_count(size_t size) {
    size_t group_bytes = SX_OFDM_DATA_SHARDS * SX_OFDM_PAYLOAD_BYTES;
    return (size + group_bytes - 1) / group_bytes;
}

size_t sx_ofdm_packet_count(size_t size) {
    return sx_ofdm_group_count(size) * SX_OFDM_TOTAL_SHARDS;
}

int sx_ofdm_make_packet(const uint8_t *data, size_t size, uint32_t image_crc,
                        size_t packet_index, uint8_t out[SX_OFDM_PACKET_BYTES]) {
    if (!data || !out || packet_index >= sx_ofdm_packet_count(size)) return 0;
    gf_init(); memset(out, 0, SX_OFDM_PACKET_BYTES);
    /* Burst-interleave physical packet order across every FEC group. A short
     * analogue dropout then costs one shard in several groups instead of five
     * adjacent shards in one group, so each 16+6 decoder can recover it. */
    size_t groups = sx_ofdm_group_count(size);
    size_t group = packet_index % groups;
    unsigned shard = (unsigned)(packet_index / groups);
    size_t group_start = group * SX_OFDM_DATA_SHARDS * SX_OFDM_PAYLOAD_BYTES;
    size_t remaining = size > group_start ? size - group_start : 0;
    unsigned data_shards = (unsigned)((remaining + SX_OFDM_PAYLOAD_BYTES - 1) / SX_OFDM_PAYLOAD_BYTES);
    if (data_shards > SX_OFDM_DATA_SHARDS) data_shards = SX_OFDM_DATA_SHARDS;
    uint8_t *payload = out + sizeof(sx_ofdm_header_t);
    uint16_t payload_size = SX_OFDM_PAYLOAD_BYTES;
    uint32_t offset = 0xffffffffu;
    uint8_t flags = 0;

    if (shard < SX_OFDM_DATA_SHARDS) {
        offset = (uint32_t)(group_start + shard * SX_OFDM_PAYLOAD_BYTES);
        if (shard >= data_shards) payload_size = 0;
        else {
            size_t available = size - offset;
            if (available < payload_size) payload_size = (uint16_t)available;
            memcpy(payload, data + offset, payload_size);
        }
    } else {
        flags = 1; unsigned row = shard - SX_OFDM_DATA_SHARDS;
        for (unsigned column = 0; column < data_shards; column++) {
            uint8_t coefficient = gf_exp[255 - gf_log[row ^ (SX_OFDM_PARITY_SHARDS + column)]];
            size_t source_offset = group_start + column * SX_OFDM_PAYLOAD_BYTES;
            size_t available = size - source_offset;
            if (available > SX_OFDM_PAYLOAD_BYTES) available = SX_OFDM_PAYLOAD_BYTES;
            for (size_t i = 0; i < available; i++) payload[i] ^= gf_mul(coefficient, data[source_offset + i]);
        }
    }
    sx_ofdm_header_t header = {
        .magic=SX_OFDM_MAGIC,.version=SX_OFDM_VERSION,.flags=flags,.group=(uint16_t)group,
        .shard=(uint8_t)shard,.data_shards=(uint8_t)data_shards,.parity_shards=SX_OFDM_PARITY_SHARDS,
        .total_size=(uint32_t)size,.offset=offset,.payload_size=payload_size,
        .packet_index=(uint16_t)packet_index,.image_crc32=image_crc,.packet_crc32=0
    };
    memcpy(out, &header, sizeof(header));
    header.packet_crc32 = sx_crc32(out, SX_OFDM_PACKET_BYTES, 0);
    memcpy(out, &header, sizeof(header));
    return 1;
}
