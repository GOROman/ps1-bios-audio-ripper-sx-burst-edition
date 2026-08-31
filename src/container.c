#include <string.h>
#include "sx_format.h"

#define LZSS_OVERHEAD(n) ((n) + (((n) + 7u) / 8u) + 1u)
static uint8_t deflate_candidate[LZSS_OVERHEAD(SX_BLOCK_SIZE)];
static sx_container_diagnostics_t sx_diagnostics;

const sx_container_diagnostics_t *sx_container_diagnostics(void) {
    return &sx_diagnostics;
}

typedef struct {
    sx_build_progress_fn callback;
    void *user;
    size_t total_size;
    size_t area_offset;
    size_t area_size;
    size_t completed_before;
    size_t stored_before;
} sx_v2_progress_t;

static void sx_v2_lzma_progress(size_t completed, size_t total, void *user) {
    sx_v2_progress_t *state = (sx_v2_progress_t *)user;
    if (!state->callback) return;
    if (completed > total) completed = total;
    size_t input_done = state->area_offset;
    if (total) input_done += (state->area_size * completed) / total;
    if (input_done < state->completed_before) input_done = state->completed_before;
    if (input_done > state->completed_before + state->area_size) {
        input_done = state->completed_before + state->area_size;
    }
    const uint16_t progress = (uint16_t)((input_done * 1000u) / state->total_size);
    state->callback(progress > 1000u ? 1000u : progress, 1000u,
                   sizeof(sx_v2_header_t) + 2u * sizeof(sx_v2_area_header_t) +
                       state->stored_before, state->user);
}

static size_t sx_build_container_v2_progress(const uint8_t *src, size_t size, uint8_t *dst,
                                             size_t cap, sx_build_progress_fn progress,
                                             void *user) {
    if (!src || !dst || size != SX_BIOS_SIZE || cap < sizeof(sx_v2_header_t) +
        2u * sizeof(sx_v2_area_header_t)) return 0;

    const uint32_t split = SX_V2_MIPS_AREA_END;
    const uint32_t offsets[2] = {0u, split};
    const uint32_t sizes[2] = {split, SX_BIOS_SIZE - split};
    const uint8_t codecs[2] = {SX_CODEC_LZMA2_MIPS, SX_CODEC_LZMA2};
    const uint8_t flags[2] = {SX_V2_AREA_FLAG_MIPS_ALIGNED, 0u};
    const uint8_t lc[2] = {2u, 3u};
    const uint8_t lp[2] = {2u, 0u};
    const uint8_t pb[2] = {2u, 2u};
    const uint8_t dict_log2 = SX_LZMA2_DICT_LOG2;
    sx_v2_area_header_t areas[2];
    const size_t table_size = 2u * sizeof(sx_v2_area_header_t);
    size_t out = sizeof(sx_v2_header_t) + table_size;
    size_t stored_payload = 0;

    for (unsigned index = 0; index < 2u; index++) {
        sx_diagnostics.lzma_attempts++;
        sx_v2_progress_t lzma_progress = {
            .callback = progress,
            .user = user,
            .total_size = size,
            .area_offset = offsets[index],
            .area_size = sizes[index],
            .completed_before = offsets[index],
            .stored_before = stored_payload
        };
        const size_t remaining = cap - out;
        size_t encoded = sx_lzma2_encode(src + offsets[index], sizes[index], dst + out, remaining,
                                         lc[index], lp[index], pb[index], dict_log2,
                                         progress ? sx_v2_lzma_progress : 0,
                                         &lzma_progress);
        uint8_t codec = codecs[index];
        uint8_t area_flags = flags[index];
        if (!encoded) {
            sx_diagnostics.lzma_failures++;
            sx_diagnostics.lzma_last_area = (uint8_t)index;
            sx_diagnostics.lzma_last_error = sx_lzma2_last_error();
            sx_diagnostics.lzma_last_request = sx_lzma2_last_request();
            return 0;
        }
        if (encoded >= sizes[index]) {
            sx_diagnostics.lzma_raw_fallbacks++;
            if (remaining < sizes[index]) return 0;
            memcpy(dst + out, src + offsets[index], sizes[index]);
            encoded = sizes[index];
            codec = SX_CODEC_RAW;
            area_flags = 0;
        }
        if (encoded > UINT32_MAX || offsets[index] + sizes[index] > size) return 0;
        sx_v2_area_header_t area = {
            .index = (uint16_t)index,
            .codec = codec,
            .flags = area_flags,
            .original_offset = offsets[index],
            .original_size = sizes[index],
            .stored_size = (uint32_t)encoded,
            .original_crc32 = sx_crc32(src + offsets[index], sizes[index], 0),
            .stored_crc32 = sx_crc32(dst + out, encoded, 0),
            .lc = codec == SX_CODEC_RAW ? 0u : lc[index],
            .lp = codec == SX_CODEC_RAW ? 0u : lp[index],
            .pb = codec == SX_CODEC_RAW ? 0u : pb[index],
            .dict_log2 = codec == SX_CODEC_RAW ? 0u : dict_log2
        };
        areas[index] = area;
        out += encoded;
        stored_payload += encoded;
        if (progress) {
            const size_t input_done = offsets[index] + sizes[index];
            progress((uint16_t)((input_done * 1000u) / size), 1000u,
                     sizeof(sx_v2_header_t) + table_size + stored_payload, user);
        }
    }

    if (out < sizeof(sx_v2_header_t) || out - sizeof(sx_v2_header_t) > UINT32_MAX) return 0;
    sx_v2_header_t header = {
        .magic = SX_MAGIC,
        .version = SX_VERSION_V2,
        .header_size = sizeof(sx_v2_header_t),
        .original_size = (uint32_t)size,
        .stored_size = (uint32_t)(out - sizeof(sx_v2_header_t)),
        .image_crc32 = sx_crc32(src, size, 0),
        .area_count = 2u,
        .flags = SX_V2_FLAG_AREA_SPLIT,
        .area_header_size = sizeof(sx_v2_area_header_t),
        .header_crc32 = 0u
    };
    header.header_crc32 = sx_crc32(&header, sizeof(header), 0);
    memcpy(dst, &header, sizeof(header));
    memcpy(dst + sizeof(header), areas, table_size);
    return out;
}

size_t sx_build_container_progress_mode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                                        sx_build_progress_fn progress, void *user,
                                        sx_container_mode_t mode) {
    memset(&sx_diagnostics, 0, sizeof(sx_diagnostics));
    if (!src || !dst || !size || size > UINT32_MAX) return 0;
    if (mode == SX_CONTAINER_V2_LZMA2) {
        return sx_build_container_v2_progress(src, size, dst, cap, progress, user);
    }
    uint16_t count = (uint16_t)((size + SX_BLOCK_SIZE - 1u) / SX_BLOCK_SIZE);
    if (cap < sizeof(sx_header_t)) return 0;
    size_t out = sizeof(sx_header_t);
    uint8_t compressed[LZSS_OVERHEAD(SX_BLOCK_SIZE)];

    for (uint16_t index = 0; index < count; index++) {
        size_t offset = (size_t)index * SX_BLOCK_SIZE;
        size_t original = size - offset;
        if (original > SX_BLOCK_SIZE) original = SX_BLOCK_SIZE;
        size_t lzss_size = sx_lzss_encode(src + offset, original, compressed, sizeof(compressed));
        size_t deflate_size = sx_deflate_fixed_encode(src + offset, original,
                                                       deflate_candidate, sizeof(deflate_candidate));
        uint8_t codec = SX_CODEC_RAW;
        const uint8_t *payload = src + offset;
        size_t stored = original;
        if (mode != SX_CONTAINER_RAW_ONLY && lzss_size && lzss_size < stored) {
            codec = SX_CODEC_LZSS; payload = compressed; stored = lzss_size;
        }
        if (mode != SX_CONTAINER_RAW_ONLY && deflate_size && deflate_size < stored) {
            codec = SX_CODEC_DEFLATE; payload = deflate_candidate; stored = deflate_size;
        }
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

static size_t sx_extract_container_v2(const uint8_t *src, size_t size, uint8_t *dst, size_t cap) {
    if (size < sizeof(sx_v2_header_t)) return 0;
    sx_v2_header_t header;
    memcpy(&header, src, sizeof(header));
    const uint32_t sent_header_crc = header.header_crc32;
    header.header_crc32 = 0;
    if (header.magic != SX_MAGIC || header.version != SX_VERSION_V2 ||
        header.header_size != sizeof(sx_v2_header_t) || header.original_size > cap ||
        header.area_count != 2u || header.flags != SX_V2_FLAG_AREA_SPLIT ||
        header.area_header_size != sizeof(sx_v2_area_header_t) ||
        header.stored_size != size - sizeof(sx_v2_header_t) ||
        sx_crc32(&header, sizeof(header), 0) != sent_header_crc) return 0;

    const size_t table_size = (size_t)header.area_count * header.area_header_size;
    const size_t table_end = sizeof(sx_v2_header_t) + table_size;
    if (table_end > size) return 0;
    const uint32_t split = SX_V2_MIPS_AREA_END;
    if (header.original_size < split) return 0;
    const uint32_t expected_offsets[2] = {0u, split};
    const uint32_t expected_sizes[2] = {split, header.original_size - split};
    size_t table_at = sizeof(sx_v2_header_t);
    size_t payload_at = table_end;
    for (unsigned index = 0; index < header.area_count; index++) {
        sx_v2_area_header_t area;
        memcpy(&area, src + table_at, sizeof(area));
        table_at += sizeof(area);
        if (area.index != index || area.original_offset != expected_offsets[index] ||
            area.original_size != expected_sizes[index] ||
            area.original_offset > header.original_size ||
            area.original_size > header.original_size - area.original_offset ||
            payload_at > size || area.stored_size > size - payload_at ||
            sx_crc32(src + payload_at, area.stored_size, 0) != area.stored_crc32) return 0;

        size_t decoded = 0;
        if (area.codec == SX_CODEC_RAW) {
            if (area.stored_size != area.original_size) return 0;
            memcpy(dst + area.original_offset, src + payload_at, area.original_size);
            decoded = area.original_size;
        } else if (area.codec == SX_CODEC_LZMA2 || area.codec == SX_CODEC_LZMA2_MIPS) {
            if (area.lc > 8u || area.lp > 4u || area.pb > 4u || area.lc + area.lp > 4u ||
                area.dict_log2 < 12u || area.dict_log2 > 27u) return 0;
            decoded = sx_lzma2_decode(src + payload_at, area.stored_size,
                                      dst + area.original_offset, area.original_size,
                                      area.dict_log2);
        } else return 0;
        if (decoded != area.original_size ||
            sx_crc32(dst + area.original_offset, decoded, 0) != area.original_crc32) return 0;
        payload_at += area.stored_size;
    }
    if (payload_at != size || sx_crc32(dst, header.original_size, 0) != header.image_crc32) return 0;
    return header.original_size;
}

size_t sx_extract_container(const uint8_t *src, size_t size, uint8_t *dst, size_t cap) {
    if (size < sizeof(sx_header_t)) return 0;
    uint32_t magic;
    uint16_t version;
    memcpy(&magic, src, sizeof(magic));
    memcpy(&version, src + sizeof(magic), sizeof(version));
    if (magic == SX_MAGIC && version == SX_VERSION_V2) {
        return sx_extract_container_v2(src, size, dst, cap);
    }
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
        } else if (block.codec == SX_CODEC_DEFLATE) {
            decoded = sx_deflate_fixed_decode(src + in, block.stored_size, dst + out, block.original_size);
        } else return 0;
        if (decoded != block.original_size || sx_crc32(dst + out, decoded, 0) != block.original_crc32) return 0;
        in += block.stored_size; out += decoded;
    }
    if (out != header.original_size || sx_crc32(dst, out, 0) != header.image_crc32) return 0;
    return out;
}
