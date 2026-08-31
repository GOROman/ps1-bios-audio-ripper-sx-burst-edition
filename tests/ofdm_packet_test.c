#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ofdm.h"
#include "sx_format.h"

int main(void) {
    const size_t size=339932; uint8_t *data=malloc(size),packet[SX_OFDM_PACKET_BYTES];
    uint8_t wire[SX_OFDM_WIRE_BYTES],restored[SX_OFDM_PACKET_BYTES];
    assert(data);for(size_t i=0;i<size;i++)data[i]=(uint8_t)(i*37u+(i>>9));
    uint32_t image_crc=sx_crc32(data,size,0);size_t count=sx_ofdm_packet_count(size);
    assert(count==1672);assert(SX_OFDM_PAYLOAD_BYTES==280);
    for(size_t index=0;index<count;index++){
        assert(sx_ofdm_make_packet(data,size,image_crc,index,packet));
        sx_ofdm_header_t header;memcpy(&header,packet,sizeof(header));uint32_t sent=header.packet_crc32;
        ((sx_ofdm_header_t *)packet)->packet_crc32=0;
        assert(sx_crc32(packet,sizeof(packet),0)==sent);
    }
    sx_inner_fec_encode(packet,wire);
    wire[17]^=0x20;
    assert(sx_inner_fec_decode(wire,restored)==1);
    assert(!memcmp(packet,restored,sizeof(packet)));
    sx_inner_fec_encode(packet,wire);
    wire[16]^=0x20;wire[17]^=0x01;
    assert(sx_inner_fec_decode(wire,restored)<0);
    printf("PASS OFDM size=%u payload=%u packets=%u groups=%u\n",(unsigned)size,
           (unsigned)SX_OFDM_PAYLOAD_BYTES,(unsigned)count,(unsigned)sx_ofdm_group_count(size));
    free(data);return 0;
}
