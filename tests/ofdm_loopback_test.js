'use strict';
/* Local transmit -> channel -> receive loopback for the audio modem.
 *
 * tests/ofdm_loopback_gen.c produces a clean 16-bit stereo capture of the PS1
 * OFDM output (modulator + SPU-ADPCM round trip).  This script layers channel
 * impairments on top -- additive white Gaussian noise, sample-clock drift, DC
 * offset, L/R gain imbalance, channel swap -- and feeds the result through the
 * real browser worker (web/ofdm-worker.js) to check that the container is
 * recovered with a matching CRC32.
 *
 *   node tests/ofdm_loopback_test.js            # clean run, asserts full recovery
 *   node tests/ofdm_loopback_test.js sweep      # impairment table, no assert
 *
 * Single-run knobs (environment):
 *   SNR_DB     AWGN level relative to signal RMS (empty = noiseless)
 *   DRIFT_PPM  sample-clock error, e.g. 150 or -150
 *   DC         DC offset added to both channels (fraction of full scale)
 *   GAIN_R     multiplier applied to the right channel (L/R imbalance)
 *   SWAP       1 to exchange L and R before decoding
 *   SEED       PRNG seed for the noise (default 1)
 *   STRICT     1 to exit non-zero when a noisy run fails to recover
 *   CHUNK      frames per postMessage (default 2048)
 *   WAV_OUT    path to also write the impaired stereo stream as a 16-bit WAV
 */
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert');
const { execFileSync } = require('node:child_process');

const GEN_BIN = '/tmp/ps1sx-loopback-gen';
const GEN_SRC = 'tests/ofdm_loopback_gen.c';
const WORKER_SRC = 'web/ofdm-worker.js';

function buildGenerator() {
    const sources = ['tests/ofdm_loopback_gen.c', 'src/crc32.c', 'src/lzss.c', 'src/container.c',
        'src/ofdm_packet.c', 'src/ofdm_mod.c', 'src/spu_adpcm.c'];
    const newest = Math.max(...sources.map(f => fs.statSync(f).mtimeMs),
        fs.existsSync('include/ofdm.h') ? fs.statSync('include/ofdm.h').mtimeMs : 0);
    if (fs.existsSync(GEN_BIN) && fs.statSync(GEN_BIN).mtimeMs > newest) return;
    const cc = process.env.CC || 'cc';
    execFileSync(cc, ['-std=c11', '-Wall', '-Wextra', '-Werror', '-DSX_HOST_TEST', '-Iinclude',
        ...sources, '-lm', '-o', GEN_BIN], { stdio: 'inherit' });
}

function readWav(path) {
    const buf = fs.readFileSync(path);
    assert.equal(buf.toString('ascii', 0, 4), 'RIFF', `${path}: not a RIFF file`);
    assert.equal(buf.toString('ascii', 8, 12), 'WAVE', `${path}: not a WAVE file`);
    let offset = 12, format = null, dataOffset = -1, dataLength = 0;
    while (offset + 8 <= buf.length) {
        const id = buf.toString('ascii', offset, offset + 4);
        const size = buf.readUInt32LE(offset + 4);
        offset += 8;
        if (id === 'fmt ') {
            format = {
                audioFormat: buf.readUInt16LE(offset),
                channels: buf.readUInt16LE(offset + 2),
                rate: buf.readUInt32LE(offset + 4),
                bits: buf.readUInt16LE(offset + 14),
            };
        } else if (id === 'data') {
            dataOffset = offset;
            dataLength = Math.min(size, buf.length - offset);
        }
        offset += size + (size & 1);
    }
    assert.ok(format, `${path}: missing fmt chunk`);
    assert.equal(format.audioFormat, 1, `${path}: not PCM`);
    assert.equal(format.bits, 16, `${path}: expected 16-bit samples`);
    assert.equal(format.channels, 2, `${path}: expected stereo`);
    assert.ok(dataOffset >= 0, `${path}: missing data chunk`);
    const frames = Math.floor(dataLength / 4);
    const left = new Float64Array(frames), right = new Float64Array(frames);
    for (let i = 0; i < frames; i++) {
        left[i] = buf.readInt16LE(dataOffset + i * 4) / 32768;
        right[i] = buf.readInt16LE(dataOffset + i * 4 + 2) / 32768;
    }
    return { left, right, frames, rate: format.rate };
}

function writeWav(path, left, right, rate) {
    const frames = Math.min(left.length, right.length);
    const buf = Buffer.alloc(44 + frames * 4);
    buf.write('RIFF', 0, 'ascii'); buf.writeUInt32LE(36 + frames * 4, 4); buf.write('WAVE', 8, 'ascii');
    buf.write('fmt ', 12, 'ascii'); buf.writeUInt32LE(16, 16); buf.writeUInt16LE(1, 20);
    buf.writeUInt16LE(2, 22); buf.writeUInt32LE(rate, 24); buf.writeUInt32LE(rate * 4, 28);
    buf.writeUInt16LE(4, 32); buf.writeUInt16LE(16, 34);
    buf.write('data', 36, 'ascii'); buf.writeUInt32LE(frames * 4, 40);
    for (let i = 0; i < frames; i++) {
        buf.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(left[i] * 32768))), 44 + i * 4);
        buf.writeInt16LE(Math.max(-32768, Math.min(32767, Math.round(right[i] * 32768))), 44 + i * 4 + 2);
    }
    fs.writeFileSync(path, buf);
}

function runGenerator() {
    const output = execFileSync(GEN_BIN, [], { encoding: 'utf8' });
    const meta = {};
    for (const line of output.trim().split('\n')) {
        const eq = line.indexOf('=');
        if (eq > 0) meta[line.slice(0, eq)] = line.slice(eq + 1);
    }
    const wav = readWav(meta.STREAM_PATH);
    assert.equal(wav.frames, Number(meta.TOTAL_FRAMES), 'WAV frame count does not match generator report');
    assert.equal(wav.rate, 44100, 'WAV sample rate is not 44100');
    return {
        left: wav.left, right: wav.right, frames: wav.frames,
        wavPath: meta.STREAM_PATH,
        container: fs.readFileSync(meta.CONTAINER_PATH),
        containerCrc: parseInt(meta.CONTAINER_CRC32, 16) >>> 0,
        packetCount: Number(meta.PACKET_COUNT),
        groupCount: Number(meta.GROUP_COUNT),
        adpcmSnr: Number(meta.ADPCM_SNR_DB),
    };
}

function mulberry32(seed) {
    let a = seed >>> 0;
    return () => {
        a |= 0; a = (a + 0x6d2b79f5) | 0;
        let t = Math.imul(a ^ (a >>> 15), 1 | a);
        t = (t + Math.imul(t ^ (t >>> 7), 61 | t)) ^ t;
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    };
}

function resample(input, ratio) {
    const outLength = Math.floor(input.length / ratio);
    const output = new Float64Array(outLength);
    for (let i = 0; i < outLength; i++) {
        const pos = i * ratio, base = Math.floor(pos), frac = pos - base;
        const a = input[base] || 0, b = input[base + 1] ?? a;
        output[i] = a + (b - a) * frac;
    }
    return output;
}

function rms(signal) {
    let sum = 0;
    for (let i = 0; i < signal.length; i++) sum += signal[i] * signal[i];
    return Math.sqrt(sum / Math.max(signal.length, 1));
}

function impair(clean, options) {
    const seed = options.seed || 1;
    let left = clean.left, right = clean.right;
    if (options.driftPpm) {
        const ratio = 1 + options.driftPpm / 1e6;
        left = resample(left, ratio);
        right = resample(right, ratio);
    }
    if (options.swap) { const t = left; left = right; right = t; }
    const outL = new Float32Array(left.length), outR = new Float32Array(right.length);
    const reference = (rms(left) + rms(right)) / 2;
    const sigma = Number.isFinite(options.snrDb) ? reference / Math.pow(10, options.snrDb / 20) : 0;
    const random = mulberry32(seed);
    const gaussian = () => {
        const u = Math.max(random(), 1e-12), v = random();
        return Math.sqrt(-2 * Math.log(u)) * Math.cos(2 * Math.PI * v);
    };
    const gainL = options.gainL ?? 1, gainR = options.gainR ?? 1, dc = options.dc || 0;
    for (let i = 0; i < left.length; i++) {
        outL[i] = Math.max(-1, Math.min(1, left[i] * gainL + dc + sigma * gaussian()));
        outR[i] = Math.max(-1, Math.min(1, right[i] * gainR + dc + sigma * gaussian()));
    }
    return { left: outL, right: outR };
}

function runWorker(pcm, options) {
    const source = fs.readFileSync(WORKER_SRC);
    const messages = [];
    const context = {
        console, performance,
        Float32Array, Float64Array, Uint8Array, Uint32Array, Int16Array, DataView,
        Map, Set, Math,
        postMessage: message => messages.push(message),
    };
    vm.createContext(context);
    vm.runInContext(source.toString(), context);
    context.onmessage({ data: { type: 'arm', index: 0, mode: options.mode || 'stereo' } });
    const chunk = options.chunk || 2048;
    for (let at = 0; at < pcm.left.length; at += chunk) {
        const left = pcm.left.slice(at, at + chunk);
        const right = pcm.right.slice(at, at + chunk);
        context.onmessage({ data: { type: 'pcm', rate: 44100, left: left.buffer, right: right.buffer } });
    }
    return messages;
}

function summarise(messages, clean) {
    const packets = messages.filter(m => m.type === 'ofdm-packet');
    const complete = [...messages].reverse().find(m => m.type === 'ofdm-complete');
    const search = [...messages].reverse().find(m => m.type === 'ofdm-search');
    const crcOk = packets.filter(m => m.valid).length;
    const crcBad = packets.filter(m => m.valid === false).length;
    const containerMatch = complete && complete.valid &&
        Buffer.from(complete.data).equals(clean.container);
    return {
        locked: messages.some(m => m.type === 'ofdm-sync'),
        crcOk, crcBad,
        packetsSeen: packets.length,
        complete: Boolean(complete),
        completeValid: Boolean(complete && complete.valid),
        containerMatch: Boolean(containerMatch),
        recoveredCrc: complete ? (complete.crc >>> 0) : null,
        bestCorrelation: search ? search.best : null,
    };
}

function describe(options) {
    const parts = [];
    parts.push(Number.isFinite(options.snrDb) ? `AWGN ${options.snrDb} dB` : 'noiseless');
    if (options.driftPpm) parts.push(`drift ${options.driftPpm > 0 ? '+' : ''}${options.driftPpm} ppm`);
    if (options.dc) parts.push(`DC ${options.dc}`);
    if (options.gainR && options.gainR !== 1) parts.push(`gain_R ${options.gainR}`);
    if (options.gainL && options.gainL !== 1) parts.push(`gain_L ${options.gainL}`);
    if (options.swap) parts.push('L/R swap');
    return parts.join(', ');
}

function once(clean, options) {
    const pcm = impair(clean, options);
    if (options.dropCount) {
        const packetFrames = 2884;
        const from = options.dropAt * packetFrames;
        const to = Math.min(pcm.left.length, from + options.dropCount * packetFrames);
        pcm.left.fill(0, from, to);
        pcm.right.fill(0, from, to);
    }
    if (options.wavOut) {
        writeWav(options.wavOut, pcm.left, pcm.right, 44100);
        process.stdout.write(`wrote impaired stream: ${options.wavOut} (${pcm.left.length} frames)\n`);
    }
    const messages = runWorker(pcm, options);
    return summarise(messages, clean);
}

function main() {
    buildGenerator();
    const clean = runGenerator();
    process.stdout.write(
        `generator: ${clean.wavPath} (${clean.frames} frames, 44100 Hz stereo)\n` +
        `           container ${clean.container.length} B / CRC ${clean.containerCrc.toString(16).padStart(8, '0')} / ` +
        `${clean.packetCount} packets / ${clean.groupCount} FEC groups / ADPCM SNR ${clean.adpcmSnr.toFixed(1)} dB\n`);

    if (process.argv[2] === 'sweep') {
        const scenarios = [
            {}, { snrDb: 42 }, { snrDb: 36 }, { snrDb: 30 }, { snrDb: 26 }, { snrDb: 22 },
            { driftPpm: 150 }, { driftPpm: -150 },
            { gainR: 0.5 }, { dc: 0.02 }, { swap: true }, { swap: true, snrDb: 30 },
        ];
        const rows = [['scenario', 'lock', 'pkt CRC ok/bad', 'complete', 'image CRC']];
        for (const scenario of scenarios) {
            const options = { snrDb: Infinity, seed: Number(process.env.SEED) || 1, ...scenario };
            const result = once(clean, options);
            rows.push([
                describe(options),
                result.locked ? 'yes' : 'no',
                `${result.crcOk}/${result.crcBad}`,
                result.containerMatch ? 'RECOVERED' : (result.complete ? 'crc mismatch' : 'no'),
                result.recoveredCrc === null ? '-' : result.recoveredCrc.toString(16).padStart(8, '0'),
            ]);
        }
        const widths = rows[0].map((_, column) => Math.max(...rows.map(row => String(row[column]).length)));
        for (const row of rows) {
            process.stdout.write(row.map((cell, column) => String(cell).padEnd(widths[column])).join('  ') + '\n');
        }
        const baseline = once(clean, { snrDb: Infinity, seed: 1 });
        assert.ok(baseline.containerMatch, 'noiseless loopback failed to recover the container');
        process.stdout.write('\nPASS loopback sweep (noiseless baseline recovered)\n');
        return;
    }

    const options = {
        snrDb: process.env.SNR_DB ? Number(process.env.SNR_DB) : Infinity,
        driftPpm: process.env.DRIFT_PPM ? Number(process.env.DRIFT_PPM) : 0,
        dc: process.env.DC ? Number(process.env.DC) : 0,
        gainR: process.env.GAIN_R ? Number(process.env.GAIN_R) : 1,
        swap: process.env.SWAP === '1',
        seed: process.env.SEED ? Number(process.env.SEED) : 1,
        chunk: process.env.CHUNK ? Number(process.env.CHUNK) : 2048,
        wavOut: process.env.WAV_OUT || null,
    };
    const result = once(clean, options);
    process.stdout.write(
        `run: ${describe(options)}\n` +
        `  carrier lock ......... ${result.locked ? 'yes' : 'no'}\n` +
        `  packet CRC ........... ${result.crcOk} ok / ${result.crcBad} bad (${result.packetsSeen} seen)\n` +
        `  best correlation ..... ${result.bestCorrelation === null ? '-' : result.bestCorrelation.toFixed(3)}\n` +
        `  container recovered .. ${result.containerMatch ? 'yes' : 'no'}\n` +
        `  image CRC ............ ${result.recoveredCrc === null ? '-' : result.recoveredCrc.toString(16).padStart(8, '0')}` +
        ` (want ${clean.containerCrc.toString(16).padStart(8, '0')})\n`);

    const noiseless = !Number.isFinite(options.snrDb) && !options.driftPpm && !options.dc &&
        options.gainR === 1 && !options.swap;
    if (noiseless || process.env.STRICT === '1') {
        assert.ok(result.containerMatch, 'loopback failed to recover the container');
        const burst = once(clean, { snrDb: Infinity, seed: 1, dropAt: 20, dropCount: 4 });
        assert.ok(burst.containerMatch,
            `four-packet burst erasure was not recovered (${burst.crcOk} ok / ${burst.crcBad} bad)`);
        process.stdout.write(`  burst erasure ......... 4 adjacent packets recovered (${burst.crcBad} CRC bad)\n`);
        process.stdout.write('PASS loopback\n');
    } else if (!result.containerMatch) {
        process.stdout.write('note: container not recovered under this impairment (informational)\n');
    } else {
        process.stdout.write('PASS loopback\n');
    }
}

main();
