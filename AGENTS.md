# PS1 BIOS Audio Ripper SX — Agent Guide

## Project goal

Back up the 512 KiB BIOS of an unmodified original PlayStation by modulating
the data onto its analogue stereo audio L/R output. A responsive browser app
captures, demodulates, validates and saves the reconstructed `ps1-bios.bin`.

This project extracts BIOS data through audio. It does not extract music or
sound assets from the BIOS.

## Reference project

Use `/Users/goroman/work/github.com/GOROman/ps1-bios-ripper-zx` as the visual
and architectural reference for the website, received-block grid, CRC status,
PS1 UI and PSn00bSDK build. ZX transports data through video; SX must keep its
audio transport and protocol independent.

## Transport baseline

- Source: 524288 bytes at `0xbfc00000`.
- Container: 16 KiB independent blocks.
- Codec: LZSS with a 4 KiB window and 18-byte maximum match; use RAW whenever
  compression does not make a block smaller.
- Integrity: stored-block CRC32, restored-block CRC32 and whole-image CRC32.
- Stable modem baseline (wire V2): 44.1 kHz stereo OFDM, 512-point FFT,
  64-sample cyclic prefix, 96 carriers from bins 24–119, eight pilots,
  QPSK and independently recoverable 16+4 FEC shards.
- Begin with a three-second channel check: 1200 Hz left-only for one second,
  1200 Hz right-only for one second, then 2400 Hz stereo for one second. Follow it with 96 alternating mark/space
  symbols for channel, polarity and sample-clock acquisition. Use 1200 Hz
  space and 2400 Hz mark at 300 baud, 147 samples/symbol at 44.1 kHz.
- Send the human-readable transfer header next using the same 300-baud BFSK,
  ASCII and 8N1 framing. End it with its CRC and a `READY` line before OFDM.
- PS1 output: fixed-point generation, SPU ADPCM and double-buffered SPU DMA.
- Browser input: AudioWorklet processing with sample-clock correction; request
  stereo with echo cancellation, noise suppression and AGC disabled.
- Provide a clearly labeled mono fallback when the capture device exposes only
  one channel.

Do not change the wire format casually. Version headers and retain decoding for
already published versions whenever practical.

## Website requirements

- Keep the web app local during development. Do not deploy or publish it until
  the user explicitly confirms that the receiver is complete and authorizes a
  release. Run and verify it through `localhost` in the meantime.
- Mobile-first responsive layout for phone portrait, phone landscape and PC.
- Follow ZX's visual hierarchy and received-block presentation.
- Keep Start/Stop and current progress visible without opening the log.
- Show actual input channel count and sample rate from track settings.
- Distinguish Missing, Receiving, Complete and CRC Error blocks by both color
  and accessible text/labels.
- Keep detailed logs collapsible on small screens.
- Never display demo data as live reception.
- Do not offer the BIOS download until every block and the final CRC32 pass.

## Validation boundary

Keep these results separate in code, documentation and status messages:

1. Static checks or codec unit tests.
2. Synthetic PCM/WAV modem loopback.
3. PS1 build or emulator execution.
4. Audio captured from a physical PlayStation and decoded in the browser.

A successful build, SPU DMA transfer, synthetic loopback or emulator run is not
proof of successful physical audio recovery. Use explicit `PENDING`, `DEMO`, or
`UNVERIFIED` labels until the corresponding layer has been observed.

## Safety and legal constraints

- Never add a PlayStation BIOS image, recovered BIOS, keys or copyrighted SDK
  files to the repository.
- Test fixtures must be generated data and must not resemble distributed BIOS
  contents.
- The output file must be written only after size and whole-image CRC32 pass.
- Keep the project framed as backup of hardware owned by the user.

## Build and checks

Host codec test:

```sh
make test
```

Serve the browser app:

```sh
make serve
```

Then open `http://localhost:8080/`. Microphone capture requires localhost or a
secure HTTPS origin.

PS1 build with a configured PSn00bSDK:

```sh
cmake --preset default
cmake --build --preset default
```

On this macOS checkout, the known working PS1 build path is the Docker
toolchain. The existing `build/CMakeCache.txt` may contain `/src/build` and
`/opt/psn00bsdk` paths created inside the container; that cache must not be
treated as a broken host build or reconfigured with the host CMake. Build it by
mounting this checkout at `/src`:

```sh
docker run --rm \
  -v /Users/goroman/work/github.com/GOROman/ps1-bios-audio-ripper-sx:/src \
  -w /src \
  luksamuk/psxtoolchain:latest \
  'cmake --build build'
```

This command updates `build/ps1sx.exe`, `build/ps1sx.bin` and
`build/ps1sx.cue`. A host-side `cmake --build --preset default` failure that
mentions `/src/build`, `/opt/psn00bsdk/bin/ninja` or a missing
`mipsel-none-elf-gcc` usually means the Docker command above should be used.
Do not launch an older CUE after source changes and describe it as the latest
build.

## Emulator launch

When launching the PS1 image in DuckStation, invoke the emulator executable
directly and pass the generated CUE file as its argument. Do not rely on Finder
file association or `open -a`, because an already-running instance may not load
the requested image. Rebuild first when PS1 sources changed, then close any old
DuckStation process before launching the new CUE.

```sh
pkill -x DuckStation 2>/dev/null || true
/Users/goroman/Applications/DuckStation.app/Contents/MacOS/DuckStation \
  /Users/goroman/work/github.com/GOROman/ps1-bios-audio-ripper-sx/build/ps1sx.cue
```

Before committing, run the relevant checks plus:

```sh
git diff --check
node --check web/app.js
node --check web/decoder-worker.js
```

## Burning a CD-R on macOS

The generated CUE refers to `ps1sx.bin` by a relative path. Run optical-disc
commands from the `build` directory so the BIN is resolved correctly:

```sh
cd /Users/goroman/work/github.com/GOROman/ps1-bios-audio-ripper-sx/build
sed -n '1,20p' ps1sx.cue
drutil status
drutil burn -verify -eject -speed 10 ps1sx.cue
```

If that drive disconnects during the verification phase, the command verified
on 2026-08-30 to complete and eject successfully is:

```sh
drutil burn -noverify -eject -speed 10 ps1sx.cue
```

`drutil` supports `.cue/bin` images and preserves the generated MODE2/2352
layout. Do not burn the BIN as an ordinary data file or synthesize a new ISO.
Use the lowest speed reported by `drutil status`; the current GX50N drive
reports 10x, 16x and 24x.

On this macOS checkout, Homebrew `cdrdao` 1.2.6 may list the USB drive with
`cdrdao scanbus` but fail to open either its IOService path or `/dev/diskN`.
Prefer `drutil` when that occurs.

Always inspect `drutil status` after the command. Some USB drives are reported
as `SupportLevel: Unsupported` and can write all image sectors before failing
during verification or eject with `The disc drive is unavailable`. In that
case, do not describe the burn as verified merely from the written-sector
count. Record separately whether:

1. The image sectors were written (`Space Used` matches the image size).
2. Post-burn verification completed.
3. The disc booted and produced audio on a physical PlayStation.

If verification repeatedly disconnects an otherwise usable drive, a later
explicitly authorized attempt may use `-noverify`, followed by an independent
readback or physical-console test. A partially written CD-R is not reusable as
a blank disc.
