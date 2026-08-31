'use strict';

const fs = require('fs');
const vm = require('vm');

function extractFunction(source, name) {
  const start = source.indexOf(`function ${name}`);
  if (start < 0) throw new Error(`${name} not found`);
  let at = source.indexOf('{', start);
  let depth = 0;
  for (; at < source.length; at++) {
    if (source[at] === '{') depth++;
    else if (source[at] === '}' && --depth === 0) return source.slice(start, at + 1);
  }
  throw new Error(`${name} is incomplete`);
}

(async () => {
  const packed = fs.readFileSync('/tmp/ps1sx-lzma2-fixture.bin');
  const bytes = fs.readFileSync('web/lzma2-decoder.wasm');
  const { instance } = await WebAssembly.instantiate(bytes, {});
  const { exports } = instance;
  const inputPtr = exports.sx_lzma2_input_ptr();
  const outputPtr = exports.sx_lzma2_output_ptr();
  new Uint8Array(exports.memory.buffer, inputPtr, packed.length).set(packed);
  const decoded = exports.sx_lzma2_decode(inputPtr, packed.length, outputPtr, 32768, 18);
  if (decoded !== 32768) throw new Error(`WASM LZMA2 output size ${decoded}`);
  const output = Buffer.from(new Uint8Array(exports.memory.buffer, outputPtr, decoded));
  const expected = Buffer.alloc(32768);
  for (let i = 0; i < expected.length; i++) {
    expected[i] = (i * 29 + (i >> 7)) & 0xff;
    if (i >= 12000 && i < 24000) expected[i] = i & 15;
  }
  if (!output.equals(expected)) throw new Error('WASM LZMA2 fixture mismatch');
  console.log(`PASS browser WASM LZMA2 ${packed.length} -> ${decoded}`);

  const app = fs.readFileSync('web/app.js', 'utf8');
  const sandbox = {};
  vm.createContext(sandbox);
  vm.runInContext(`${extractFunction(app, 'crc32')};${extractFunction(app, 'lzma2DecodeArea')};${extractFunction(app, 'validateV2Container')};${extractFunction(app, 'reconstructBios')};this.rebuild=reconstructBios`, sandbox);
  const container = fs.readFileSync('/tmp/ps1sx-v2-container.bin');
  const result = sandbox.rebuild(container.buffer.slice(container.byteOffset, container.byteOffset + container.byteLength), { instance });
  if (!result.ok) throw new Error(`browser V2 LZMA2 rebuild failed: ${result.error}`);
  const image = Buffer.alloc(524288);
  for (let i = 0; i < image.length; i++) {
    image[i] = (i * 29 + (i >> 8)) & 0xff;
    if (i >= 120000 && i < 360000) image[i] = i & 31;
  }
  if (!Buffer.from(result.image).equals(image)) throw new Error('browser V2 LZMA2 image mismatch');
  console.log(`PASS browser V2 LZMA2 container ${container.length} -> ${result.image.length}`);
})().catch(error => {
  console.error(error);
  process.exitCode = 1;
});
