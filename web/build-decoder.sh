#!/bin/sh
# Build web/ofdm-decoder.wasm from src/ofdm_demod.c and the independent
# browser LZMA2 decoder used by SX V2.
#
# Freestanding wasm32: no libc, no emscripten runtime.  ofdm-worker.js
# instantiates the module with an empty import object and reads/writes the
# exported static buffers directly through `memory`.
set -e
cd "$(dirname "$0")"

CLANG=${CLANG:-$(command -v clang || true)}
for c in \
  /opt/homebrew/Cellar/emscripten/*/libexec/llvm/bin/clang \
  /opt/homebrew/opt/llvm/bin/clang \
  /usr/local/opt/llvm/bin/clang
do
  [ -x "$c" ] && CLANG=$c && break
done
[ -n "$CLANG" ] || { echo "no clang found" >&2; exit 1; }

"$CLANG" --target=wasm32 -O3 -ffreestanding -fno-builtin -nostdlib -fno-exceptions \
  -Wall -Wextra -Werror \
  -Wl,--no-entry -Wl,--allow-undefined \
  -Wl,--initial-memory=16777216 \
  -Wl,--export=memory \
  -Wl,--export=sxd_reset \
  -Wl,--export=sxd_set_mode \
  -Wl,--export=sxd_process \
  -Wl,--export=sxd_in_l \
  -Wl,--export=sxd_in_r \
  -Wl,--export=sxd_out \
  -Wl,--export=sxd_in_capacity \
  -Wl,--export=sxd_out_capacity \
  -Wl,--export=sxd_out_stride \
  -Wl,--export=sxd_carrier_detected \
  -Wl,--export=sxd_crc_ok \
  -Wl,--export=sxd_crc_bad \
  -Wl,--export=sxd_drops \
  -Wl,--export=sxd_received \
  -Wl,--export=sxd_buffered \
  -o ofdm-decoder.wasm ../src/ofdm_demod.c

ls -l ofdm-decoder.wasm

"$CLANG" --target=wasm32 -O3 -ffreestanding -fno-builtin -nostdlib -fno-exceptions \
  -Wall -Wextra -Werror -DZ7_ST -I. -I../include -I../src -I../src/lzma \
  -Wl,--no-entry -Wl,--allow-undefined \
  -Wl,--initial-memory=16777216 \
  -Wl,--export=memory \
  -Wl,--export=sx_lzma2_decode \
  -Wl,--export=sx_lzma2_input_ptr \
  -Wl,--export=sx_lzma2_output_ptr \
  -o lzma2-decoder.wasm lzma2_decoder.c ../src/lzma/Lzma2Dec.c ../src/lzma/LzmaDec.c

ls -l lzma2-decoder.wasm
