.PHONY: test calibration serve analyze-ofdm
test:
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude tests/codec_test.c src/crc32.c src/lzss.c src/container.c -o /tmp/ps1sx-codec-test
	/tmp/ps1sx-codec-test
	$(CC) -std=c11 -Wall -Wextra -Werror -Iinclude tests/ofdm_packet_test.c src/crc32.c src/ofdm_packet.c -o /tmp/ps1sx-ofdm-packet-test
	/tmp/ps1sx-ofdm-packet-test
	$(CC) -std=c11 -Wall -Wextra -Werror -DSX_HOST_TEST -Iinclude tests/ofdm_mod_test.c src/crc32.c src/ofdm_packet.c src/ofdm_mod.c -lm -o /tmp/ps1sx-ofdm-mod-test
	/tmp/ps1sx-ofdm-mod-test
	$(CC) -std=c11 -Wall -Wextra -Werror -DSX_HOST_TEST -Iinclude tests/spu_adpcm_test.c src/crc32.c src/ofdm_packet.c src/ofdm_mod.c src/spu_adpcm.c -lm -o /tmp/ps1sx-spu-adpcm-test
	/tmp/ps1sx-spu-adpcm-test
	node tests/ofdm_worker_test.js
	node tests/ofdm_rs_test.js
	$(CC) -std=c11 -Wall -Wextra -Werror -DSX_HOST_TEST -Iinclude tests/ofdm_loopback_gen.c src/crc32.c src/lzss.c src/container.c src/ofdm_packet.c src/ofdm_mod.c src/spu_adpcm.c -lm -o /tmp/ps1sx-loopback-gen
	node tests/ofdm_loopback_test.js
	$(CC) -std=c11 -Wall -Wextra -Werror tests/ofdm_demod_test.c -lm -o /tmp/ps1sx-ofdm-demod-test
	/tmp/ps1sx-ofdm-demod-test
	/tmp/ps1sx-ofdm-demod-test /tmp/ps1sx-loopback.wav 288400

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

analyze-ofdm:
	$(CXX) -std=c++17 -Wall -Wextra -Werror tools/ofdm_wav_decode.cpp -lm -o /tmp/ps1sx-ofdm-wav-decode
	/tmp/ps1sx-ofdm-wav-decode --mono /tmp/ps1sx-ofdm-first.wav
