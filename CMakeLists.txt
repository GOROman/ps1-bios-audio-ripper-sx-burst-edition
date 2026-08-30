cmake_minimum_required(VERSION 3.21)
project(ps1_bios_audio_ripper_sx LANGUAGES C ASM)

psn00bsdk_add_executable(ps1sx GPREL
    src/main.c src/marker_tim.c src/crc32.c src/lzss.c src/container.c src/audio_tx.c src/ofdm_packet.c src/ofdm_mod.c src/ofdm_tx.c src/spu_adpcm.c
)
target_include_directories(ps1sx PRIVATE include)
target_link_libraries(ps1sx PRIVATE psxgpu psxspu psxgte psxetc psxapi)
psn00bsdk_add_cd_image(ps1sx_iso ps1sx iso.xml DEPENDS ps1sx SYSTEM.CNF)
