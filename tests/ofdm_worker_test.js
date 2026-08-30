const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert');
const source=fs.readFileSync('web/ofdm-worker.js'),raw=fs.readFileSync('/tmp/ps1sx-ofdm-adpcm.raw');
const left=new Float32Array(2888),right=new Float32Array(2888);
for(let i=0;i<2880;i++){left[i]=raw.readInt16LE(i*4)/32768;right[i]=raw.readInt16LE(i*4+2)/32768}
const messages=[],context={console,performance,Float32Array,Float64Array,Uint8Array,DataView,Map,Set,Math,postMessage:m=>messages.push(m)};
vm.createContext(context);vm.runInContext(source.toString(),context);
context.onmessage({data:{type:'disarm'}});
context.onmessage({data:{type:'pcm',rate:44100,left:left.buffer.slice(0),right:right.buffer.slice(0)}});
assert.equal(messages.filter(message=>message.type==='ofdm-packet').length,0,'disarmed worker decoded OFDM before FSK');
context.onmessage({data:{type:'arm',index:3,mode:'stereo'}});
context.onmessage({data:{type:'pcm',rate:44100,left:left.buffer,right:right.buffer}});
const packet=messages.find(message=>message.type==='ofdm-packet');
assert(packet,'worker did not find an OFDM packet');
assert.equal(packet.valid,true,`packet CRC failed: score=${packet.score} snr=${packet.snr}`);
assert.equal(packet.index,3);assert.equal(packet.group,3);assert.equal(packet.shard,0);
console.log(`PASS browser OFDM worker packet=${packet.index} correlation=${packet.score.toFixed(3)} SNR=${packet.snr.toFixed(1)} dB`);

const monoRaw=fs.readFileSync('/tmp/ps1sx-ofdm-mono.raw'),monoLeft=new Float32Array(5192),monoRight=new Float32Array(5192);
for(let i=0;i<5184;i++){monoLeft[i]=monoRaw.readInt16LE(i*4)/32768;monoRight[i]=monoRaw.readInt16LE(i*4+2)/32768}
const monoMessages=[],monoContext={console,performance,Float32Array,Float64Array,Uint8Array,DataView,Map,Set,Math,postMessage:m=>monoMessages.push(m)};
vm.createContext(monoContext);vm.runInContext(source.toString(),monoContext);
monoContext.onmessage({data:{type:'arm',index:3,mode:'mono'}});
monoContext.onmessage({data:{type:'pcm',rate:44100,left:monoLeft.buffer,right:monoRight.buffer}});
const monoPacket=monoMessages.find(message=>message.type==='ofdm-packet');
assert(!monoPacket?.valid,'Burst worker must reject legacy mono OFDM');
console.log('PASS browser Burst stereo-only mode rejects mono OFDM');
