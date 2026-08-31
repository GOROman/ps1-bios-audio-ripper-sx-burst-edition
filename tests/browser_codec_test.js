'use strict';

const fs = require('fs');
const vm = require('vm');
const zlib = require('zlib');

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

const app = fs.readFileSync('web/app.js', 'utf8');
const sandbox = {};
vm.createContext(sandbox);
vm.runInContext(`${extractFunction(app, 'crc32')};${extractFunction(app, 'fixedDeflateDecode')};${extractFunction(app, 'lzssDecode')};${extractFunction(app, 'lzma2DecodeArea')};${extractFunction(app, 'validateV2Container')};${extractFunction(app, 'reconstructBios')};this.decode=fixedDeflateDecode;this.rebuild=reconstructBios;this.checkV2=validateV2Container`, sandbox);

const input = Buffer.alloc(16 * 1024);
for (let i = 0; i < input.length; i++) input[i] = (i >> 3) ^ (i * 17);
const packed = zlib.deflateRawSync(input, {level: 9, strategy: zlib.constants.Z_FIXED});
const output = Buffer.from(sandbox.decode(packed, input.length));
if (!output.equals(input)) throw new Error(`fixed Deflate mismatch: ${output.length} bytes`);
console.log(`PASS browser fixed Deflate ${packed.length} -> ${output.length}`);

const v2Input = Buffer.alloc(524288);
for (let i = 0; i < v2Input.length; i++) v2Input[i] = (i * 29 + (i >> 8)) & 0xff;
const split = 0x044c60;
const v2Container = new Uint8Array(32 + 56 + v2Input.length);
const v2View = new DataView(v2Container.buffer);
v2View.setUint32(0, 0x58533150, true);
v2View.setUint16(4, 2, true);
v2View.setUint16(6, 32, true);
v2View.setUint32(8, v2Input.length, true);
v2View.setUint32(12, 56 + v2Input.length, true);
v2View.setUint32(16, sandbox.crc32(v2Input), true);
v2View.setUint16(20, 2, true);
v2View.setUint16(22, 1, true);
v2View.setUint32(24, 28, true);
let tableAt = 32, payloadAt = 88;
for (const [index, end] of [split, v2Input.length].entries()) {
  const begin = index ? split : 0;
  const size = end - begin;
  v2View.setUint16(tableAt, index ? 1 : 0, true);
  v2Container[tableAt + 2] = 0;
  v2View.setUint32(tableAt + 4, begin, true);
  v2View.setUint32(tableAt + 8, size, true);
  v2View.setUint32(tableAt + 12, size, true);
  v2View.setUint32(tableAt + 16, sandbox.crc32(v2Input.subarray(begin, end)), true);
  v2View.setUint32(tableAt + 20, sandbox.crc32(v2Input.subarray(begin, end)), true);
  v2Container.set(v2Input.subarray(begin, end), payloadAt);
  tableAt += 28;
  payloadAt += size;
}
v2View.setUint32(28, 0, true);
v2View.setUint32(28, sandbox.crc32(v2Container.subarray(0, 32)), true);
const v2Validation = sandbox.checkV2(v2Container);
if (!v2Validation.ok) throw new Error(`V2 validation mismatch: ${v2Validation.error}`);
const v2Result = sandbox.rebuild(v2Container.buffer);
if (!v2Result.ok || !Buffer.from(v2Result.image).equals(v2Input)) throw new Error('V2 RAW compatibility mismatch');
console.log(`PASS browser V2 RAW compatibility ${v2Container.length} -> ${v2Result.image.length}`);
