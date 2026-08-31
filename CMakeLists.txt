cmake_minimum_required(VERSION 3.21)
project(ps1_bios_audio_ripper_sx LANGUAGES C ASM)

option(SX_TEST_AUTOSTART "Start the Burst transmit path automatically in emulator test builds" OFF)
set(SX_BLOCK_SIZE "" CACHE STRING "Override the independent container block size")
set(SX_DEFLATE_MAX_CHAIN "" CACHE STRING "Override the fixed-Deflate match search depth")

psn00bsdk_add_executable(ps1sx GPREL
    src/main.c src/marker_tim.c src/crc32.c src/lzss.c src/deflate_fixed.c src/container.c src/audio_tx.c src/ofdm_packet.c src/inner_fec.c src/ofdm_mod.c src/ofdm_tx.c src/spu_adpcm.c
)
target_include_directories(ps1sx PRIVATE include)
if(SX_TEST_AUTOSTART)
    target_compile_definitions(ps1sx PRIVATE SX_TEST_AUTOSTART)
endif()
if(SX_BLOCK_SIZE)
    target_compile_definitions(ps1sx PRIVATE SX_BLOCK_SIZE=${SX_BLOCK_SIZE})
endif()
if(SX_DEFLATE_MAX_CHAIN)
    target_compile_definitions(ps1sx PRIVATE SX_DEFLATE_MAX_CHAIN=${SX_DEFLATE_MAX_CHAIN})
endif()
target_link_libraries(ps1sx PRIVATE psxgpu psxspu psxgte psxetc psxapi)
psn00bsdk_add_cd_image(ps1sx_iso ps1sx iso.xml DEPENDS ps1sx SYSTEM.CNF)
