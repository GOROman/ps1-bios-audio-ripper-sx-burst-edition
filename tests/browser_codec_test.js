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
vm.runInContext(`${extractFunction(app, 'crc32')};${extractFunction(app, 'fixedDeflateDecode')};${extractFunction(app, 'lzssDecode')};${extractFunction(app, 'reconstructBios')};this.decode=fixedDeflateDecode;this.rebuild=reconstructBios`, sandbox);

const input = Buffer.alloc(16 * 1024);
for (let i = 0; i < input.length; i++) input[i] = (i >> 3) ^ (i * 17);
const packed = zlib.deflateRawSync(input, {level: 9, strategy: zlib.constants.Z_FIXED});
const output = Buffer.from(sandbox.decode(packed, input.length));
if (!output.equals(input)) throw new Error(`fixed Deflate mismatch: ${output.length} bytes`);
console.log(`PASS browser fixed Deflate ${packed.length} -> ${output.length}`);

const v2Header = new ArrayBuffer(32);
const v2View = new DataView(v2Header);
v2View.setUint32(0, 0x58533150, true);
v2View.setUint16(4, 2, true);
const v2Result = sandbox.rebuild(v2Header);
if (v2Result.ok || !String(v2Result.error).includes('PENDING')) throw new Error('V2 dispatch mismatch');
console.log('PASS browser V2 LZMA2 dispatch is explicit PENDING');
