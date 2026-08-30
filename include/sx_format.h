#pragma once

#include <stddef.h>
#include <stdint.h>

#define SX_BIOS_ADDR       0xbfc00000u
#define SX_BIOS_SIZE       524288u
#define SX_BLOCK_SIZE      16384u
#define SX_MAGIC           0x58533150u /* "P1SX" little endian */
#define SX_VERSION         1u
#define SX_CODEC_RAW       0u
#define SX_CODEC_LZSS      1u

typedef enum {
    SX_CONTAINER_MIXED = 0,
    SX_CONTAINER_RAW_ONLY = 1
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

uint32_t sx_crc32(const void *data, size_t size, uint32_t seed);
size_t sx_lzss_encode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
size_t sx_lzss_decode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
size_t sx_build_container(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
typedef void (*sx_build_progress_fn)(uint16_t completed, uint16_t total, size_t stored_bytes, void *user);
size_t sx_build_container_progress(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                                   sx_build_progress_fn progress, void *user);
size_t sx_build_container_progress_mode(const uint8_t *src, size_t size, uint8_t *dst, size_t cap,
                                        sx_build_progress_fn progress, void *user,
                                        sx_container_mode_t mode);
size_t sx_extract_container(const uint8_t *src, size_t size, uint8_t *dst, size_t cap);
