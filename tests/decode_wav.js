'use strict';
/* Decode an arbitrary stereo WAV recording through the real browser OFDM worker.
 *
 *   node tests/decode_wav.js <recording.wav> [reference-bios.bin]
 *
 * Feed it a line-in / emulator capture of the PS1 audio output.  The worker
 * (web/ofdm-worker.js) searches for the OFDM burst, demodulates every packet and
 * reassembles the SX container.  With a reference BIOS the recovered container's
 * first block is checked against the first 16 KiB of that file.
 *
 * The capture must be 16-bit PCM stereo.  Any sample rate is accepted; the
 * worker resamples to its internal 44.1 kHz.
 */
const fs = require('node:fs');
const vm = require('node:vm');

function readWav(path) {
    const buf = fs.readFileSync(path);
    if (buf.toString('ascii', 0, 4) !== 'RIFF' || buf.toString('ascii', 8, 12) !== 'WAVE') {
        throw new Error(`${path}: not a RIFF/WAVE file`);
    }
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
    if (!format) throw new Error(`${path}: no fmt chunk`);
    if (format.audioFormat !== 1 && format.audioFormat !== 0xfffe) {
        throw new Error(`${path}: not PCM (format ${format.audioFormat})`);
    }
    if (format.bits !== 16) throw new Error(`${path}: expected 16-bit samples, got ${format.bits}`);
    if (dataOffset < 0) throw new Error(`${path}: no data chunk`);
    const step = format.channels;
    const frames = Math.floor(dataLength / (2 * step));
    const left = new Float32Array(frames), right = new Float32Array(frames);
    for (let i = 0; i < frames; i++) {
        left[i] = buf.readInt16LE(dataOffset + i * 2 * step) / 32768;
        right[i] = buf.readInt16LE(dataOffset + i * 2 * step + (step > 1 ? 2 : 0)) / 32768;
    }
    return { left, right, frames, rate: format.rate, channels: format.channels };
}

function crc32(bytes) {
    let crc = 0xffffffff;
    for (const value of bytes) {
        crc ^= value;
        for (let bit = 0; bit < 8; bit++) crc = (crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
    }
    return (crc ^ 0xffffffff) >>> 0;
}

function runWorker(wav, chunk) {
    const source = fs.readFileSync('web/ofdm-worker.js');
    const messages = [];
    const context = {
        console, performance,
        Float32Array, Float64Array, Uint8Array, Uint32Array, Int16Array, DataView,
        Map, Set, Math,
        postMessage: message => messages.push(message),
    };
    vm.createContext(context);
    vm.runInContext(source.toString(), context);
    context.onmessage({ data: { type: 'arm', index: 0 } });
    for (let at = 0; at < wav.left.length; at += chunk) {
        const left = wav.left.slice(at, at + chunk);
        const right = wav.right.slice(at, at + chunk);
        context.onmessage({ data: { type: 'pcm', rate: wav.rate, left: left.buffer, right: right.buffer } });
    }
    return messages;
}

function validateContainer(container, referenceBios) {
    if (container.length < 28) return { ok: false, error: 'container too short' };
    const view = new DataView(container.buffer, container.byteOffset, container.byteLength);
    if (view.getUint32(0, true) !== 0x58533150) return { ok: false, error: 'bad SX magic' };
    const blocks = view.getUint16(22, true);
    let at = view.getUint16(6, true), complete = 0;
    const report = [];
    for (let index = 0; index < blocks; index++) {
        if (at + 16 > container.length) return { ok: false, error: `block ${index} header missing` };
        const blockIndex = view.getUint16(at, true);
        const codec = container[at + 2];
        const originalSize = view.getUint16(at + 4, true);
        const storedSize = view.getUint16(at + 6, true);
        const originalCrc = view.getUint32(at + 8, true);
        const storedCrc = view.getUint32(at + 12, true);
        at += 16;
        if (at + storedSize > container.length) return { ok: false, error: `block ${index} payload missing` };
        const stored = container.subarray(at, at + storedSize);
        const storedOk = crc32(stored) === storedCrc;
        let restoredOk = null;
        if (referenceBios) {
            const slice = referenceBios.subarray(blockIndex * 16384, blockIndex * 16384 + originalSize);
            restoredOk = crc32(slice) === originalCrc;
        }
        report.push({ blockIndex, codec: codec === 1 ? 'LZSS' : (codec === 2 ? 'DEFLATE' : 'RAW'), storedSize, originalSize, storedOk, restoredOk });
        if (storedOk && restoredOk !== false) complete++;
        at += storedSize;
    }
    return { ok: complete === blocks, blocks, complete, report };
}

function main() {
    const wavPath = process.argv[2];
    const biosPath = process.argv[3];
    if (!wavPath) {
        process.stderr.write('usage: node tests/decode_wav.js <recording.wav> [reference-bios.bin]\n');
        process.exit(2);
    }
    const wav = readWav(wavPath);
    const seconds = wav.frames / wav.rate;
    let rms = 0, peak = 0;
    for (let i = 0; i < wav.frames; i++) {
        const l = wav.left[i]; rms += l * l; peak = Math.max(peak, Math.abs(l), Math.abs(wav.right[i]));
    }
    rms = Math.sqrt(rms / Math.max(wav.frames, 1));
    process.stdout.write(
        `capture: ${wavPath}\n` +
        `  ${wav.channels} ch / ${wav.rate} Hz / ${wav.frames} frames / ${seconds.toFixed(2)} s\n` +
        `  level: RMS ${(20 * Math.log10(Math.max(rms, 1e-9))).toFixed(1)} dBFS / peak ${(20 * Math.log10(Math.max(peak, 1e-9))).toFixed(1)} dBFS\n`);
    if (peak < 1e-4) {
        process.stdout.write('  -> capture is silent; nothing to decode\n');
        process.exit(1);
    }

    const referenceBios = biosPath ? new Uint8Array(fs.readFileSync(biosPath)) : null;
    const messages = runWorker(wav, 4096);
    const packets = messages.filter(m => m.type === 'ofdm-packet');
    const crcOk = packets.filter(m => m.valid).length;
    const crcBad = packets.filter(m => m.valid === false).length;
    const carrier = messages.find(m => m.type === 'ofdm-carrier');
    const search = [...messages].reverse().find(m => m.type === 'ofdm-search');
    const complete = [...messages].reverse().find(m => m.type === 'ofdm-complete');

    process.stdout.write(
        `\ndecode:\n` +
        `  carrier detect ...... ${carrier ? `${carrier.frequency} Hz (coherence ${carrier.coherence.toFixed(3)})` : 'no'}\n` +
        `  sync locks .......... ${messages.filter(m => m.type === 'ofdm-sync').length}\n` +
        `  packet CRC .......... ${crcOk} ok / ${crcBad} bad\n` +
        `  best correlation .... ${search ? search.best.toFixed(3) : (packets[0] ? packets[0].score.toFixed(3) : '-')}\n` +
        `  container assembled . ${complete ? 'yes' : 'no'}\n`);

    if (!complete) {
        process.stdout.write('  -> not enough clean packets to reassemble the container\n');
        process.exit(1);
    }
    const container = new Uint8Array(complete.data);
    const result = validateContainer(container, referenceBios);
    process.stdout.write(
        `  image CRC (worker) .. ${(complete.crc >>> 0).toString(16).padStart(8, '0')}` +
        ` vs expected ${(complete.expectedCrc >>> 0).toString(16).padStart(8, '0')} -> ${complete.valid ? 'match' : 'MISMATCH'}\n` +
        `  container ........... ${container.length} bytes / ${result.blocks ?? '?'} block(s)\n`);
    for (const block of result.report || []) {
        process.stdout.write(
            `    block ${block.blockIndex}: ${block.codec} stored ${block.storedSize} / orig ${block.originalSize}` +
            ` / stored CRC ${block.storedOk ? 'OK' : 'BAD'}` +
            (block.restoredOk === null ? '' : ` / vs reference BIOS ${block.restoredOk ? 'OK' : 'BAD'}`) + '\n');
    }
    const pass = complete.valid && result.ok && (!referenceBios || result.report.every(b => b.restoredOk));
    process.stdout.write(pass ? '\nPASS decode_wav\n' : '\nFAIL decode_wav\n');
    process.exit(pass ? 0 : 1);
}

main();
