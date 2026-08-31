cmake_minimum_required(VERSION 3.21)
project(ps1_bios_audio_ripper_sx LANGUAGES C ASM)

option(SX_TEST_AUTOSTART "Start the Burst transmit path automatically in emulator test builds" OFF)
set(SX_BLOCK_SIZE "" CACHE STRING "Override the independent container block size")
set(SX_DEFLATE_MAX_CHAIN "" CACHE STRING "Override the fixed-Deflate match search depth")
set(SX_CONTAINER_VERSION "1" CACHE STRING "Container version emitted by the PS1 sender (1 or 2)")

psn00bsdk_add_executable(ps1sx GPREL
    src/main.c src/marker_tim.c src/crc32.c src/lzss.c src/deflate_fixed.c src/container.c
    src/lzma2_encode.c src/lzma2_decode.c
    src/lzma/CpuArch.c src/lzma/LzFind.c src/lzma/Lzma2Enc.c src/lzma/LzmaEnc.c
    src/lzma/Lzma2Dec.c src/lzma/LzmaDec.c
    src/audio_tx.c src/ofdm_packet.c src/inner_fec.c src/ofdm_mod.c src/ofdm_tx.c src/spu_adpcm.c
)
target_include_directories(ps1sx PRIVATE include src src/lzma)
target_compile_definitions(ps1sx PRIVATE
    Z7_ST SX_LZMA_NO_ERRNO SX_LZMA_LOW_MEMORY SX_LZMA_TAIL_ARENA
    SX_LZMA2_DICT_LOG2=16 SX_LZMA_ALGO=1 SX_LZMA_BT_MODE=0
    SX_LZMA_HASH_BYTES=3 SX_LZMA_HASH_BITS=16 SX_LZMA_FB=192 SX_LZMA_MC=128
    SX_LZMA_INTERNAL_PROGRESS
)
if(SX_TEST_AUTOSTART)
    target_compile_definitions(ps1sx PRIVATE SX_TEST_AUTOSTART)
endif()
if(SX_BLOCK_SIZE)
    target_compile_definitions(ps1sx PRIVATE SX_BLOCK_SIZE=${SX_BLOCK_SIZE})
endif()
if(SX_DEFLATE_MAX_CHAIN)
    target_compile_definitions(ps1sx PRIVATE SX_DEFLATE_MAX_CHAIN=${SX_DEFLATE_MAX_CHAIN})
endif()
if(SX_CONTAINER_VERSION STREQUAL "2")
    target_compile_definitions(ps1sx PRIVATE SX_DEFAULT_CONTAINER_V2)
elseif(NOT SX_CONTAINER_VERSION STREQUAL "1")
    message(FATAL_ERROR "SX_CONTAINER_VERSION must be 1 or 2")
endif()
target_link_libraries(ps1sx PRIVATE psxgpu psxspu psxgte psxetc psxapi)
psn00bsdk_target_incbin(ps1sx PRIVATE fanfare_adpcm assets/benkirb-fanfare-1.adpcm)
psn00bsdk_add_cd_image(ps1sx_iso ps1sx iso.xml DEPENDS ps1sx SYSTEM.CNF)
