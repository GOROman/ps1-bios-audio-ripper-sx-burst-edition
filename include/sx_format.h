#pragma once

#include <stddef.h>
#include <stdint.h>

#define SX_BIOS_ADDR       0xbfc00000u
#define SX_BIOS_SIZE       524288u
#ifndef SX_BLOCK_SIZE
#define SX_BLOCK_SIZE      16384u
#endif
#define SX_MAGIC           0x58533150u /* "P1SX" little endian */
#define SX_VERSION         1u
#define SX_VERSION_V1      1u
#define SX_VERSION_V2      2u
#define SX_CODEC_RAW       0u
#define SX_CODEC_LZSS      1u
#define SX_CODEC_DEFLATE   2u
#define SX_CODEC_LZMA2     3u
#define SX_CODEC_LZMA2_MIPS 4u

/* V2 keeps the same transport framing but describes two independently
 * decodable areas.  V1 remains the default sender for old captures. */
#define SX_V2_FLAG_AREA_SPLIT       0x0001u
#define SX_V2_AREA_FLAG_MIPS_ALIGNED 0x01u
#define SX_V2_MIPS_AREA_END         0x044c60u
#ifndef SX_LZMA2_DICT_LOG2
#define SX_LZMA2_DICT_LOG2           18u
#endif

typedef enum {
    SX_CONTAINER_MIXED = 0,
    SX_CONTAINER_RAW_ONLY = 1,
    SX_CONTAINER_V2_LZMA2 = 2
} sx_container_mode_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t original_size;
    uint32_t stored_size;
    uint32_t image_crc32;
    uint16_t block_size;
    uint16_t block_count;
    uint32_t header_crc32;
} sx_header_t;

typedef struct __attribute__((packed)) {
    uint16_t index;
    uint8_t codec;
    uint8_t flags;
    uint16_t original_size;
    uint16_t stored_size;
    uint32_t original_crc32;
    uint32_t stored_crc32;
} sx_block_header_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_size;
    uint32_t original_size;
    uint32_t stored_size;
    uint32_t image_crc32;
    uint16_t area_count;
    uint16_t flags;
    uint32_t area_header_size;
    uint32_t header_crc32;
} sx_v2_header_t;

typedef struct __attribute__((packed)) {
    uint16_t index;
    uint8_t codec;
    uint8_t flags;
    uint32_t original_offset;
    uint32_t original_size;
    uint32_t stored_size;
    uint32_t original_crc32;
    uint32_t stored_crc32;
    uint8_t lc;
    uint8_t lp;
    uint8_t pb;
    uint8_t dict_log2;
} sx_v2_area_header_t;

_Static_assert(sizeof(sx_v2_header_t) == 32, "SX V2 header must stay 32 bytes");
_Static_assert(sizeof(sx_v2_area_header_t) == 28, "SX V2 area header must stay 28 bytes");

uint32_t sx_crc32(const void *data, size_t size, uint32_t seed);
size_t sx_lzss_encode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
size_t sx_lzss_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
size_t sx_deflate_fixed_encode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
size_t sx_deflate_fixed_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
size_t sx_build_container(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
typedef void (*sx_build_progress_fn)(uint16_t completed, uint16_t total, size_t stored_bytes, void *user);
typedef void (*sx_lzma_progress_fn)(size_t completed_bytes, size_t total_bytes, void *user);

typedef struct {
    uint8_t lzma_attempts;
    uint8_t lzma_failures;
    uint8_t lzma_raw_fallbacks;
    uint8_t lzma_last_area;
    int lzma_last_error;
    size_t lzma_last_request;
} sx_container_diagnostics_t;

size_t sx_lzma2_encode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                       uint8_t lc, uint8_t lp, uint8_t pb, uint8_t dict_log2,
                       sx_lzma_progress_fn progress, void *user);
int sx_lzma2_last_error(void);
size_t sx_lzma2_last_request(void);
size_t sx_lzma2_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                       uint8_t dict_log2);

size_t sx_build_container_progress(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                                   sx_build_progress_fn progress, void *user);
size_t sx_build_container_progress_mode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                                        sx_build_progress_fn progress, void *user,
                                        sx_container_mode_t mode);
const sx_container_diagnostics_t *sx_container_diagnostics(void);
size_t sx_extract_container(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
