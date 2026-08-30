#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ofdm.h"
#include "sx_format.h"

int main(void) {
    const size_t size=339932; uint8_t *data=malloc(size),packet[SX_OFDM_PACKET_BYTES];
    assert(data);for(size_t i=0;i<size;i++)data[i]=(uint8_t)(i*37u+(i>>9));
    uint32_t image_crc=sx_crc32(data,size,0);size_t count=sx_ofdm_packet_count(size);
    assert(count==2664);assert(SX_OFDM_PAYLOAD_BYTES==144);
    for(size_t index=0;index<count;index++){
        assert(sx_ofdm_make_packet(data,size,image_crc,index,packet));
        sx_ofdm_header_t header;memcpy(&header,packet,sizeof(header));uint32_t sent=header.packet_crc32;
        ((sx_ofdm_header_t *)packet)->packet_crc32=0;
        assert(sx_crc32(packet,sizeof(packet),0)==sent);
    }
    printf("PASS OFDM size=%u payload=%u packets=%u groups=%u\n",(unsigned)size,
           (unsigned)SX_OFDM_PAYLOAD_BYTES,(unsigned)count,(unsigned)sx_ofdm_group_count(size));
    free(data);return 0;
}
