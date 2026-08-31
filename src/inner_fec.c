#include "ofdm.h"

#include <string.h>

static unsigned get_bit(const uint8_t *data, unsigned bit) {
    return (data[bit >> 3] >> (bit & 7u)) & 1u;
}

static void set_bit(uint8_t *data, unsigned bit, unsigned value) {
    uint8_t mask = (uint8_t)(1u << (bit & 7u));
    if (value) data[bit >> 3] |= mask;
    else data[bit >> 3] &= (uint8_t)~mask;
}

void sx_inner_fec_encode(const uint8_t input[SX_OFDM_PACKET_BYTES],
                         uint8_t output[SX_OFDM_WIRE_BYTES]) {
    memset(output, 0, SX_OFDM_WIRE_BYTES);
    for (unsigned group = 0; group < SX_OFDM_PACKET_BYTES / 8u; group++) {
        uint8_t *code = output + group * 9u;
        unsigned data_bit = 0;
        for (unsigned position = 1; position <= 71; position++) {
            if (!(position & (position - 1u))) continue;
            set_bit(code, position - 1u, get_bit(input + group * 8u, data_bit++));
        }
        for (unsigned parity = 1; parity <= 64; parity <<= 1) {
            unsigned value = 0;
            for (unsigned position = 1; position <= 71; position++)
                if ((position & parity) && position != parity) value ^= get_bit(code, position - 1u);
            set_bit(code, parity - 1u, value);
        }
        unsigned overall = 0;
        for (unsigned position = 1; position <= 71; position++) overall ^= get_bit(code, position - 1u);
        set_bit(code, 71u, overall);
    }
}

int sx_inner_fec_decode(const uint8_t input[SX_OFDM_WIRE_BYTES],
                        uint8_t output[SX_OFDM_PACKET_BYTES]) {
    memset(output, 0, SX_OFDM_PACKET_BYTES);
    int corrected = 0;
    for (unsigned group = 0; group < SX_OFDM_PACKET_BYTES / 8u; group++) {
        uint8_t code[9]; memcpy(code, input + group * 9u, sizeof(code));
        unsigned syndrome = 0, overall = 0;
        for (unsigned position = 1; position <= 71; position++) {
            if (get_bit(code, position - 1u)) syndrome ^= position;
            overall ^= get_bit(code, position - 1u);
        }
        overall ^= get_bit(code, 71u);
        if (syndrome) {
            if (!overall || syndrome > 71u) return -1;
            set_bit(code, syndrome - 1u, !get_bit(code, syndrome - 1u));
            corrected++;
        } else if (overall) {
            corrected++;
        }
        unsigned data_bit = 0;
        for (unsigned position = 1; position <= 71; position++) {
            if (!(position & (position - 1u))) continue;
            set_bit(output + group * 8u, data_bit++, get_bit(code, position - 1u));
        }
    }
    return corrected;
}
