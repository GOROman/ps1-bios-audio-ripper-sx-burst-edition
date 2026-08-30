#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include "ofdm.h"
#include "ofdm_mod.h"
#include "sx_format.h"

int main(void) {
    uint8_t source[20000],packet[SX_OFDM_PACKET_BYTES];
    int16_t left[SX_OFDM_PACKET_SAMPLES],right[SX_OFDM_PACKET_SAMPLES];
    static int16_t mono_left[SX_OFDM_MONO_PACKET_SAMPLES],mono_right[SX_OFDM_MONO_PACKET_SAMPLES];
    for(unsigned i=0;i<sizeof(source);i++)source[i]=(uint8_t)(i*29u+7u);
    assert(sx_ofdm_make_packet(source,sizeof(source),sx_crc32(source,sizeof(source),0),3,packet));
    sx_ofdm_modulate_packet(packet,3,left,right);
    sx_ofdm_modulate_packet_mono(packet,3,mono_left,mono_right);
    int mono_nonzero=0;
    for(unsigned i=0;i<SX_OFDM_MONO_PACKET_SAMPLES;i++){
        assert(mono_left[i]==mono_right[i]);
        if(mono_left[i])mono_nonzero=1;
    }
    assert(mono_nonzero);
    FILE *file=fopen("/tmp/ps1sx-ofdm-fixed.raw","wb");assert(file);
    for(unsigned i=0;i<SX_OFDM_PACKET_SAMPLES;i++){fwrite(&left[i],2,1,file);fwrite(&right[i],2,1,file);}fclose(file);
    file=fopen("/tmp/ps1sx-ofdm-mono.raw","wb");assert(file);
    for(unsigned i=0;i<SX_OFDM_MONO_PACKET_SAMPLES;i++){fwrite(&mono_left[i],2,1,file);fwrite(&mono_right[i],2,1,file);}fclose(file);
    file=fopen("/tmp/ps1sx-ofdm-packet.bin","wb");assert(file);fwrite(packet,1,sizeof(packet),file);fclose(file);
    printf("PASS fixed OFDM stereo=%u mono=%u samples\n",SX_OFDM_PACKET_SAMPLES,SX_OFDM_MONO_PACKET_SAMPLES);return 0;
}
