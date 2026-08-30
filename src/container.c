#include <string.h>
#include "sx_format.h"

#define LZSS_OVERHEAD(n) ((n) + (((n) + 7u) / 8u) + 1u)

size_t sx_build_container_progress_mode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                                        sx_build_progress_fn progress, void *user,
                                        sx_container_mode_t mode) {
    if (!src || !dst || !size || size > UINT32_MAX) return 0;
    uint16_t count = (uint16_t)((size + SX_BLOCK_SIZE - 1u) / SX_BLOCK_SIZE);
    if (cap < sizeof(sx_header_t)) return 0;
    size_t out = sizeof(sx_header_t);
    uint8_t compressed[LZSS_OVERHEAD(SX_BLOCK_SIZE)];

    for (uint16_t index = 0; index < count; index++) {
        size_t offset = (size_t)index * SX_BLOCK_SIZE;
        size_t original = size - offset;
        if (original > SX_BLOCK_SIZE) original = SX_BLOCK_SIZE;
        size_t packed = sx_lzss_encode(src + offset, original, compressed, sizeof(compressed));
        uint8_t codec = mode == SX_CONTAINER_RAW_ONLY ? SX_CODEC_RAW :
                        (packed && packed < original ? SX_CODEC_LZSS : SX_CODEC_RAW);
        const uint8_t *payload = codec == SX_CODEC_LZSS ? compressed : src + offset;
        size_t stored = codec == SX_CODEC_LZSS ? packed : original;
        if (out + sizeof(sx_block_header_t) + stored > cap) return 0;
        sx_block_header_t block = {
            .index = index, .codec = codec, .flags = 0,
            .original_size = (uint16_t)original, .stored_size = (uint16_t)stored,
            .original_crc32 = sx_crc32(src + offset, original, 0),
            .stored_crc32 = sx_crc32(payload, stored, 0)
        };
        memcpy(dst + out, &block, sizeof(block)); out += sizeof(block);
        memcpy(dst + out, payload, stored); out += stored;
        if (progress) progress((uint16_t)(index + 1u), count, out, user);
    }

    sx_header_t header = {
        .magic = SX_MAGIC, .version = SX_VERSION, .header_size = sizeof(sx_header_t),
        .original_size = (uint32_t)size,
        .stored_size = (uint32_t)(out - sizeof(sx_header_t)),
        .image_crc32 = sx_crc32(src, size, 0),
        .block_size = SX_BLOCK_SIZE, .block_count = count, .header_crc32 = 0
    };
    header.header_crc32 = sx_crc32(&header, sizeof(header), 0);
    memcpy(dst, &header, sizeof(header));
    return out;
}

size_t sx_build_container_progress(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                                   sx_build_progress_fn progress, void *user) {
    return sx_build_container_progress_mode(src, size, dst, cap, progress, user, SX_CONTAINER_MIXED);
}

size_t sx_build_container(const uint8_t *src, size_t size, uint8_t *dst, size_t cap) {
    return sx_build_container_progress(src, size, dst, cap, 0, 0);
}

size_t sx_extract_container(const uint8_t *src, size_t size, uint8_t *dst, size_t cap) {
    if (size < sizeof(sx_header_t)) return 0;
    sx_header_t header; memcpy(&header, src, sizeof(header));
    uint32_t sent_header_crc = header.header_crc32; header.header_crc32 = 0;
    if (header.magic != SX_MAGIC || header.version != SX_VERSION ||
        header.header_size != sizeof(sx_header_t) || header.original_size > cap ||
        sx_crc32(&header, sizeof(header), 0) != sent_header_crc) return 0;
    size_t in = sizeof(sx_header_t), out = 0;
    for (uint16_t index = 0; index < header.block_count; index++) {
        if (in + sizeof(sx_block_header_t) > size) return 0;
        sx_block_header_t block; memcpy(&block, src + in, sizeof(block)); in += sizeof(block);
        if (block.index != index || in + block.stored_size > size ||
            out + block.original_size > cap ||
            sx_crc32(src + in, block.stored_size, 0) != block.stored_crc32) return 0;
        size_t decoded;
        if (block.codec == SX_CODEC_RAW) {
            if (block.stored_size != block.original_size) return 0;
            memcpy(dst + out, src + in, block.stored_size); decoded = block.stored_size;
        } else if (block.codec == SX_CODEC_LZSS) {
            decoded = sx_lzss_decode(src + in, block.stored_size, dst + out, block.original_size);
        } else return 0;
        if (decoded != block.original_size || sx_crc32(dst + out, decoded, 0) != block.original_crc32) return 0;
        in += block.stored_size; out += decoded;
    }
    if (out != header.original_size || sx_crc32(dst, out, 0) != header.image_crc32) return 0;
    return out;
}
