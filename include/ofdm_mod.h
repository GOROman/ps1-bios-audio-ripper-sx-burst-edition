#pragma once
#include <stdint.h>
#include "ofdm.h"

#define SX_OFDM_PACKET_SAMPLES (SX_OFDM_SYMBOLS_PACKET * (SX_OFDM_FFT_SIZE + SX_OFDM_CP_SIZE))
#define SX_OFDM_MONO_SYMBOLS_PACKET 9u
#define SX_OFDM_MONO_PACKET_SAMPLES (SX_OFDM_MONO_SYMBOLS_PACKET * (SX_OFDM_FFT_SIZE + SX_OFDM_CP_SIZE))
#define SX_OFDM_MAX_PACKET_SAMPLES SX_OFDM_MONO_PACKET_SAMPLES

void sx_ofdm_modulate_packet(const uint8_t packet[SX_OFDM_PACKET_BYTES], uint32_t packet_index,
                             int16_t left[SX_OFDM_PACKET_SAMPLES],
                             int16_t right[SX_OFDM_PACKET_SAMPLES]);
void sx_ofdm_modulate_packet_mono(const uint8_t packet[SX_OFDM_PACKET_BYTES], uint32_t packet_index,
                                  int16_t left[SX_OFDM_MONO_PACKET_SAMPLES],
                                  int16_t right[SX_OFDM_MONO_PACKET_SAMPLES]);
