.PHONY: test calibration serve analyze-ofdm codec-sweep codec-sweep-test v2-lzma-test v2-lzma-encode
LZMA2_CFLAGS = -DZ7_ST -DSX_LZMA_LOW_MEMORY -DSX_LZMA2_DICT_LOG2=16 \
	-DSX_LZMA_ALGO=1 -DSX_LZMA_BT_MODE=0 -DSX_LZMA_HASH_BYTES=3 \
	-DSX_LZMA_HASH_BITS=16 -DSX_LZMA_FB=192 -DSX_LZMA_MC=128 \
	-DSX_LZMA_INTERNAL_PROGRESS -Isrc/lzma
LZMA2_SOURCES = src/lzma2_encode.c src/lzma2_decode.c src/lzma/CpuArch.c src/lzma/Lzma2Enc.c src/lzma/LzmaEnc.c src/lzma/LzFind.c src/lzma/Lzma2Dec.c src/lzma/LzmaDec.c
test:
	$(CC) -std=c11 -Wall -Wextra -Werror $(LZMA2_CFLAGS) -Iinclude tests/codec_test.c src/crc32.c src/lzss.c src/deflate_fixed.c src/container.c $(LZMA2_SOURCES) -o /tmp/ps1sx-codec-test
	/tmp/ps1sx-codec-test
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude tests/ofdm_packet_test.c src/crc32.c src/inner_fec.c src/ofdm_packet.c -o /tmp/ps1sx-ofdm-packet-test
	/tmp/ps1sx-ofdm-packet-test
	$(CC) -std=c11 -Wall -Wextra -Werror -DSX_HOST_TEST -Iinclude tests/ofdm_mod_test.c src/crc32.c src/ofdm_packet.c src/inner_fec.c src/ofdm_mod.c -lm -o /tmp/ps1sx-ofdm-mod-test
	/tmp/ps1sx-ofdm-mod-test
	$(CC) -std=c11 -Wall -Wextra -Werror -DSX_HOST_TEST -Iinclude tests/spu_adpcm_test.c src/crc32.c src/ofdm_packet.c src/inner_fec.c src/ofdm_mod.c src/spu_adpcm.c -lm -o /tmp/ps1sx-spu-adpcm-test
	/tmp/ps1sx-spu-adpcm-test
	node tests/ofdm_worker_test.js
	node tests/ofdm_rs_test.js
	node tests/browser_codec_test.js
	$(CC) -std=c11 -Wall -Wextra -Werror $(LZMA2_CFLAGS) -Iinclude tests/lzma2_fixture.c src/crc32.c src/lzss.c src/deflate_fixed.c src/container.c $(LZMA2_SOURCES) -o /tmp/ps1sx-lzma2-fixture
	/tmp/ps1sx-lzma2-fixture
	node tests/lzma2_wasm_test.js
	python3 tools/codec_sweep.py --self-test
	$(CC) -std=c11 -Wall -Wextra -Werror -DSX_HOST_TEST $(LZMA2_CFLAGS) -Iinclude tests/ofdm_loopback_gen.c src/crc32.c src/lzss.c src/deflate_fixed.c src/container.c $(LZMA2_SOURCES) src/ofdm_packet.c src/inner_fec.c src/ofdm_mod.c src/spu_adpcm.c -lm -o /tmp/ps1sx-loopback-gen
	node tests/ofdm_loopback_test.js
	$(CC) -std=c11 -Wall -Wextra -Werror tests/ofdm_demod_test.c -lm -o /tmp/ps1sx-ofdm-demod-test
	/tmp/ps1sx-ofdm-demod-test

.PHONY: loopback
loopback:
	node tests/ofdm_loopback_test.js sweep

calibration:
	python3 tools/modem_ref.py /tmp/ps1sx-fsk-header.wav

.PHONY: ofdm-selftest
ofdm-selftest:
	python3 tools/ofdm_ref.py selftest

serve:
	python3 -m http.server 8080 -d web

codec-sweep:
	python3 tools/codec_sweep.py $(ARGS)

codec-sweep-test:
	python3 tools/codec_sweep.py --self-test

v2-lzma-test:
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Iinclude -I/opt/homebrew/include -L/opt/homebrew/lib tools/sx_v2_lzma.cpp -llzma -o /tmp/ps1sx-v2-lzma
	/tmp/ps1sx-v2-lzma --self-test

v2-lzma-encode:
	$(CXX) -std=c++17 -Wall -Wextra -Werror -Iinclude -I/opt/homebrew/include -L/opt/homebrew/lib tools/sx_v2_lzma.cpp -llzma -o /tmp/ps1sx-v2-lzma
	/tmp/ps1sx-v2-lzma $(ARGS)

analyze-ofdm:
	$(CXX) -std=c++17 -Wall -Wextra -Werror tools/ofdm_wav_decode.cpp -lm -o /tmp/ps1sx-ofdm-wav-decode
	/tmp/ps1sx-ofdm-wav-decode --mono /tmp/ps1sx-ofdm-first.wav
