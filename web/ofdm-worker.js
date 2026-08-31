const RATE=44100,FFT=512,CP=32,SYMBOL=FFT+CP,PACKET_SAMPLES=SYMBOL*5,MONO_PACKET_SAMPLES=SYMBOL*9;
const FIRST_BIN=24,CARRIERS=96,PILOTS=new Set([0,13,27,41,55,69,83,95]),PAYLOAD=280,DATA_SHARDS=16,PARITY_SHARDS=6,GROUP_SHARDS=DATA_SHARDS+PARITY_SHARDS,MAGIC=0x314f5853;
const LEVELS=[-3/Math.sqrt(10),-1/Math.sqrt(10),3/Math.sqrt(10),1/Math.sqrt(10)];

const LOCK_WINDOW=160; /* packet stride minus PACKET_SAMPLES (SPU ADPCM padding) plus clock drift */
let armed=false,inputRate=RATE,decoderMode='stereo',currentBlock=0,fecEpoch=0;
let pcmPort=null,preRoll=[];
const PRE_ROLL_CHUNKS=8;

/* The DSP hot path (resample + sync search + per-symbol FFT/equalise) is ported
 * to src/ofdm_demod.c and compiled to web/ofdm-decoder.wasm.  The pure-JS path
 * below is kept as a fallback; it decodes identically but stalls under CPU load.
 * Group assembly + container CRC stay in JS on both paths. */
let W=null;
const PKT_STRIDE=376; /* sizeof(sxd_packet_out_t) */
(async()=>{
  try{
    const res=await fetch(new URL('ofdm-decoder.wasm?v=11',self.location.href));
    if(!res.ok)throw new Error('fetch '+res.status);
    const {instance}=await WebAssembly.instantiate(await res.arrayBuffer(),{});
    W=instance.exports;
    if(W.sxd_out_stride()!==PKT_STRIDE)throw new Error('stride '+W.sxd_out_stride());
    if(armed)W.sxd_reset(expectedPacket);
    postMessage({type:'ofdm-wasm',ok:true});
  }catch(e){
    W=null;
    postMessage({type:'ofdm-wasm',ok:false,error:String(e&&e.message||e)});
  }
})();
function drainWasm(count){
  if(!count)return;
  const buf=W.memory.buffer,base=W.sxd_out();
  for(let k=0;k<count;k++){
    const o=base+k*PKT_STRIDE,dv=new DataView(buf,o,PKT_STRIDE);
    const data=new Uint8Array(buf.slice(o,o+312));
    const valid=dv.getInt32(312,true)!==0;
    /* cfo/phase are host-decoder diagnostics not carried in the WASM struct;
     * the app reads them with .toFixed() so they must be present. */
    const score=dv.getFloat32(360,true),evm=dv.getFloat32(364,true),snr=dv.getFloat32(368,true),
      timing=dv.getFloat32(372,true),swap=dv.getInt32(356,true)!==0,cfo=0,phase=0;
    const packetIndex=dv.getUint32(316,true);lastPacketSeen=Math.max(lastPacketSeen,packetIndex);
    if(!valid){crcBad++;drops++;postMessage({type:'ofdm-packet',valid:false,index:packetIndex,crcOk,crcBad,drops,score,evm,snr,timing,swap,cfo,phase});continue}
    crcOk++;if(!started)started=performance.now();
    const group=dv.getUint32(320,true),shard=dv.getInt32(344,true),dataShards=dv.getInt32(348,true),
      total=dv.getUint32(324,true),offset=dv.getUint32(328,true),size=dv.getInt32(352,true);
    totalSize=total;imageCrc=dv.getUint32(332,true);
    if(!groups.has(group))groups.set(group,{k:dataShards,shards:new Map(),parity:new Map()});
    const grp=groups.get(group);
    if(shard<dataShards)grp.shards.set(shard,data.slice(32));
    else if(shard>=DATA_SHARDS)grp.parity.set(shard-DATA_SHARDS,data.slice(32));
    postMessage({type:'ofdm-sync',correlation:score,index:packetIndex});
    postMessage({type:'ofdm-packet',valid:true,index:packetIndex,group,shard,dataShards,total,offset,size,crcOk,crcBad,drops,score,evm,snr,timing,swap,cfo,phase});
    assemble();
  }
  if(totalSize&&lastPacketSeen+1>=Math.ceil(totalSize/(DATA_SHARDS*PAYLOAD))*GROUP_SHARDS)finalizeRecovery();
}
function appendWasm(left,right,rate){
  const cap=W.sxd_in_capacity();
  for(let off=0;off<left.length;){
    const n=Math.min(cap,left.length-off);
    const bl=new Float32Array(W.memory.buffer,W.sxd_in_l(),n);
    const br=new Float32Array(W.memory.buffer,W.sxd_in_r(),n);
    for(let i=0;i<n;i++){bl[i]=left[off+i];br[i]=right[off+i]??left[off+i]}
    drainWasm(W.sxd_process(n,rate||RATE));
    off+=n;
  }
  if(!carrierReported&&W.sxd_carrier_detected()){carrierReported=true;postMessage({type:'ofdm-carrier',frequency:6000,coherence:1})}
}
let audioL=[],audioR=[],expectedPacket=0,crcOk=0,crcBad=0,drops=0,started=0,totalSize=0,imageCrc=0,reportAt=0,receivedSamples=0,carrierReported=false,locked=false,lastPacketSeen=-1,completeSent=false;
const groups=new Map();
const failedGroups=new Set();
const FEC_WORKERS=2,fecPool=[];
function failFec(group,index,missing){if(group?.fecFailed)return;group.fecFailed=true;postMessage({type:'ofdm-fec-failed',group:index,missing,parity:group?.parity?.size||0})}
function reportThreads(){postMessage({type:'ofdm-threads',main:1,workers:2+fecPool.filter(Boolean).length,fec:fecPool.filter(Boolean).length})}
if(typeof Worker!=='undefined')for(let index=0;index<FEC_WORKERS;index++){
  try{
    const worker=new Worker('fec-worker.js?v=4');fecPool[index]=worker;
    worker.onmessage=event=>{
      const message=event.data;if(message.type==='ready'){reportThreads();return}
      if(message.epoch!==fecEpoch)return;
      const group=groups.get(message.group);if(!group)return;group.fecPending=false;
      if(message.ok){for(const [shard,data] of message.shards)group.shards.set(shard,new Uint8Array(data));if(!group.fecLogged){group.fecLogged=true;postMessage({type:'ofdm-fec',group:message.group,recovered:message.recovered,parity:group.parity.size,worker:index})}assemble()}else failFec(group,message.group,group.k-group.shards.size)
    };
    worker.onerror=()=>{fecPool[index]=null;reportThreads()};
  }catch{fecPool[index]=null}
}
reportThreads();

function requestRsRecover(group,groupIndex){
  const missing=group.k-group.shards.size;if(missing<=0)return true;
  if(group.parity.size<missing)return false;
  const worker=fecPool[currentBlock%FEC_WORKERS];
  if(!worker)return rsRecover(group,groupIndex);
  if(group.fecPending)return false;
  group.fecPending=true;
  const shards=[...group.shards].map(([index,data])=>[index,Uint8Array.from(data)]),parity=[...group.parity].map(([index,data])=>[index,Uint8Array.from(data)]);
  worker.postMessage({epoch:fecEpoch,block:currentBlock,group:groupIndex,k:group.k,shards,parity});
  return false;
}
function finalizeRecovery(){
  if(!totalSize)return;
  const count=Math.ceil(totalSize/(DATA_SHARDS*PAYLOAD));
  for(let index=0;index<count;index++){
    const group=groups.get(index);if(!group){if(!failedGroups.has(index)){failedGroups.add(index);postMessage({type:'ofdm-fec-failed',group:index,missing:16,parity:0})}continue}
    const missing=group.k-group.shards.size;if(missing<=0||group.fecPending)continue;
    if(group.parity.size>=missing)requestRsRecover(group,index);else failFec(group,index,missing);
  }
}

/* Windowed-sinc (Blackman, 24-tap) resampler: capture rate -> RATE.  Linear
 * interpolation of a 48 kHz mic feed smeared the OFDM carriers enough to fail
 * every packet; this keeps the passband flat to the 44.1 kHz Nyquist. */
const RS_HALF=12;
let rsL=[],rsR=[],rsBase=0,rsPos=RS_HALF;
function winSinc(x,cutoff){
  if(x<=-RS_HALF||x>=RS_HALF)return 0;
  const px=Math.PI*x;
  const s=x===0?cutoff:Math.sin(cutoff*px)/px;
  const w=0.42+0.5*Math.cos(px/RS_HALF)+0.08*Math.cos(2*px/RS_HALF);
  return s*w;
}

function crc32(data){let crc=0xffffffff;for(const value of data){crc^=value;for(let bit=0;bit<8;bit++)crc=(crc>>>1)^((crc&1)?0xedb88320:0)}return (crc^0xffffffff)>>>0}
function whiten(data,index){let state=(0x9e3779b9^index)>>>0,out=new Uint8Array(data.length);for(let i=0;i<data.length;i++){state^=state<<13;state^=state>>>17;state^=state<<5;state>>>=0;out[i]=data[i]^(state&255)}return out}
function innerFecDecode(wire){const packet=new Uint8Array(312),bit=(a,n)=>(a[n>>3]>>(n&7))&1,set=(a,n,v)=>{const m=1<<(n&7);a[n>>3]=v?a[n>>3]|m:a[n>>3]&~m};let corrected=0;for(let g=0;g<39;g++){const code=wire.slice(g*9,g*9+9);let syndrome=0,overall=0;for(let p=1;p<=71;p++){if(bit(code,p-1))syndrome^=p;overall^=bit(code,p-1)}overall^=bit(code,71);if(syndrome){if(!overall||syndrome>71)return null;set(code,syndrome-1,!bit(code,syndrome-1));corrected++}else if(overall)corrected++;let d=0;for(let p=1;p<=71;p++){if(!(p&(p-1)))continue;set(packet.subarray(g*8,g*8+8),d++,bit(code,p-1))}}return{packet,corrected}}

/* Reed-Solomon-style erasure decode over GF(256), poly 0x11d, matching the
 * transmitter in src/ofdm_packet.c.  Parity row r (0..5), data column c (0..k-1)
 * uses the Cauchy coefficient A(r,c) = 1 / (r XOR (6 + c)).  With e data shards
 * missing and >= e parity shards received we solve the e x e system for the gaps
 * byte-wise.  Runs once per group off the hot path, so plain JS is fine. */
const gfExp=new Uint8Array(512),gfLog=new Uint8Array(256);
{let v=1;for(let i=0;i<255;i++){gfExp[i]=v;gfLog[v]=i;v<<=1;if(v&0x100)v^=0x11d}for(let i=255;i<512;i++)gfExp[i]=gfExp[i-255]}
const gfMul=(a,b)=>(a&&b)?gfExp[gfLog[a]+gfLog[b]]:0;
const gfInv=a=>gfExp[255-gfLog[a]];
function rsSolve(B,S,e,L){
  for(let col=0;col<e;col++){
    let piv=col;while(piv<e&&!B[piv][col])piv++;
    if(piv===e)return false;
    if(piv!==col){const tb=B[piv];B[piv]=B[col];B[col]=tb;const ts=S[piv];S[piv]=S[col];S[col]=ts}
    const inv=gfInv(B[col][col]);
    for(let j=col;j<e;j++)B[col][j]=gfMul(B[col][j],inv);
    for(let i=0;i<L;i++)S[col][i]=gfMul(S[col][i],inv);
    for(let r=0;r<e;r++){
      if(r===col||!B[r][col])continue;
      const f=B[r][col];
      for(let j=col;j<e;j++)B[r][j]^=gfMul(f,B[col][j]);
      for(let i=0;i<L;i++)S[r][i]^=gfMul(f,S[col][i]);
    }
  }
  return true;
}
function rsRecover(group,g){
  const k=group.k,missing=[];
  for(let c=0;c<k;c++)if(!group.shards.has(c))missing.push(c);
  const e=missing.length;
  if(e===0)return true;
  if(!group.parity||group.parity.size<e)return false;
  const L=PAYLOAD,prows=[...group.parity.keys()].sort((a,b)=>a-b).slice(0,e),B=[],S=[];
  for(let j=0;j<e;j++){
    const r=prows[j],s=Uint8Array.from(group.parity.get(r));
    for(let c=0;c<k;c++){const sh=group.shards.get(c);if(!sh)continue;const coef=gfInv(r^(6+c));for(let i=0;i<L;i++)s[i]^=gfMul(coef,sh[i])}
    const row=new Uint8Array(e);
    for(let t=0;t<e;t++)row[t]=gfInv(r^(6+missing[t]));
    B.push(row);S.push(s);
  }
  if(!rsSolve(B,S,e,L))return false;
  for(let t=0;t<e;t++)group.shards.set(missing[t],S[t]);
  if(!group.fecLogged){group.fecLogged=true;postMessage({type:'ofdm-fec',group:g,recovered:e,parity:group.parity.size})}
  return true;
}
function fft(re,im){const n=re.length;for(let i=1,j=0;i<n;i++){let bit=n>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j){[re[i],re[j]]=[re[j],re[i]];[im[i],im[j]]=[im[j],im[i]]}}for(let len=2;len<=n;len<<=1){const angle=-2*Math.PI/len;for(let base=0;base<n;base+=len){for(let j=0;j<len/2;j++){const wr=Math.cos(angle*j),wi=Math.sin(angle*j),at=base+j+len/2,ar=re[at]*wr-im[at]*wi,ai=re[at]*wi+im[at]*wr,bt=base+j,br=re[bt],bi=im[bt];re[bt]=br+ar;im[bt]=bi+ai;re[at]=br-ar;im[at]=bi-ai}}}}

function makeSync(){const out=[];for(let n=-CP;n<FFT;n++){let value=0,at=(n+FFT)%FFT;for(let c=0;c<CARRIERS;c++){const k=FIRST_BIN+c,sign=((k*73+19)&1)?-1:1;value+=2*sign*Math.cos(2*Math.PI*k*at/FFT)}out.push(value)}const power=Math.sqrt(out.reduce((s,v)=>s+v*v,0));return out.map(v=>v/power)}
const sync=makeSync();
function correlation(at){let dotL=0,dotR=0,powL=0,powR=0;for(let i=0;i<SYMBOL;i++){const l=audioL[at+i],r=audioR[at+i],s=sync[i];dotL+=l*s;dotR+=r*s;powL+=l*l;powR+=r*r}return Math.max(Math.abs(dotL)/Math.sqrt(powL||1),Math.abs(dotR)/Math.sqrt(powR||1))}
/* Scan [lo,hi] and return the strongest correlation.  The sync peak is only ~2
 * samples wide, so a coarse stride can skip it entirely; once a strong candidate
 * is seen we refine to sample accuracy around it and take that earliest peak
 * rather than a later, equally strong peak from the following packet. */
function scanRange(lo,hi,step){let bestAt=lo,bestScore=-1;for(let at=lo;at<=hi;at+=step){const value=correlation(at);if(value>bestScore){bestScore=value;bestAt=at}if(value>=.6){for(let a=Math.max(lo,at-step);a<=Math.min(hi,at+step);a++){const refined=correlation(a);if(refined>bestScore){bestScore=refined;bestAt=a}}return {at:bestAt,score:bestScore}}}return {at:bestAt,score:bestScore}}
function findSync(){if(audioL.length<PACKET_SAMPLES+4)return null;const end=Math.min(audioL.length-PACKET_SAMPLES-4,4096);
  /* Packets arrive back to back, so after a lock the next sync sits just past 0.
   * Search a short window at full resolution to avoid latching the packet after. */
  if(locked){const near=scanRange(0,Math.min(end,LOCK_WINDOW),1);if(near.score>=.5)return near;locked=false}
  const found=scanRange(0,end,2);
  if(found.score<.42){if(receivedSamples>=reportAt){let sum=0,peak=0,start=Math.max(0,audioL.length-1024);for(let i=start;i<audioL.length;i++){const value=audioL[i];sum+=value*value;peak=Math.max(peak,Math.abs(value))}postMessage({type:'ofdm-search',samples:receivedSamples,buffered:audioL.length,best:found.score,rms:Math.sqrt(sum/Math.max(audioL.length-start,1)),peak,drops});reportAt=receivedSamples+8192}if(audioL.length>PACKET_SAMPLES+8192){const drop=audioL.length-(PACKET_SAMPLES+8192);audioL.splice(0,drop);audioR.splice(0,drop);drops++}return null}
  let {at,score}=found;for(let a=Math.max(0,at-4);a<=Math.min(end,at+4);a++){const value=correlation(a);if(value>score){score=value;at=a}}return {at,score}}
function unwrap(values){for(let i=1;i<values.length;i++){while(values[i]-values[i-1]>Math.PI)values[i]-=2*Math.PI;while(values[i]-values[i-1]<-Math.PI)values[i]+=2*Math.PI}return values}
function fit(xs,ys){const n=xs.length,mx=xs.reduce((a,b)=>a+b,0)/n,my=ys.reduce((a,b)=>a+b,0)/n;let top=0,bot=0;for(let i=0;i<n;i++){top+=(xs[i]-mx)*(ys[i]-my);bot+=(xs[i]-mx)**2}const slope=top/bot;return {slope,intercept:my-slope*mx}}
function median(values){const s=[...values].sort((a,b)=>a-b);return (s[(s.length-1)>>1]+s[s.length>>1])/2}
function nearest(value){let best=0,error=Math.abs(value-LEVELS[0]);for(let i=1;i<4;i++){const e=Math.abs(value-LEVELS[i]);if(e<error){error=e;best=i}}return best}
function estimateChannel(source,at){const re=new Float64Array(FFT),im=new Float64Array(FFT);for(let i=0;i<FFT;i++)re[i]=source[at+CP+i];fft(re,im);for(let carrier=0;carrier<CARRIERS;carrier++){const bin=FIRST_BIN+carrier,sign=((bin*73+19)&1)?-1:1;re[bin]*=sign;im[bin]*=sign}return{re,im}}
function equalize(source,base,h){const re=new Float64Array(FFT),im=new Float64Array(FFT),er=new Float64Array(FFT),ei=new Float64Array(FFT);for(let i=0;i<FFT;i++)re[i]=source[base+i];fft(re,im);for(let carrier=0;carrier<CARRIERS;carrier++){const bin=FIRST_BIN+carrier,den=Math.max(h.re[bin]**2+h.im[bin]**2,1e-18);er[bin]=(re[bin]*h.re[bin]+im[bin]*h.im[bin])/den;ei[bin]=(im[bin]*h.re[bin]-re[bin]*h.im[bin])/den}return{re:er,im:ei}}
function mrc(a,b,ha,hb,bin){const pa=ha.re[bin]**2+ha.im[bin]**2,pb=hb.re[bin]**2+hb.im[bin]**2,den=Math.max(pa+pb,1e-18);return{re:(a.re[bin]*pa+b.re[bin]*pb)/den,im:(a.im[bin]*pa+b.im[bin]*pb)/den}}
function cpCfo(base){let re=0,im=0;for(let ch=0;ch<2;ch++){const a=ch?audioR:audioL;for(let i=0;i<CP;i++){const x=a[base+i],y=a[base+i+FFT];re+=x*y;/* real audio cannot expose complex CFO; phase remains diagnostic */}}return Math.atan2(im,re)*RATE/(2*Math.PI*FFT)}
function detectCarrier(){if(carrierReported||audioL.length<512)return;let re=0,im=0,power=0,start=audioL.length-512,step=2*Math.PI*6000/RATE;for(let i=0;i<512;i++){const value=audioL[start+i];re+=value*Math.cos(step*i);im-=value*Math.sin(step*i);power+=value*value}const coherence=(re*re+im*im)/Math.max(power*512,1e-12);if(coherence>.22){carrierReported=true;postMessage({type:'ofdm-carrier',frequency:6000,coherence})}}
function decode(at,index,score,swap=false,quiet=false){const scrambled=new Uint8Array(352),sources=swap?[audioR,audioL]:[audioL,audioR],channels=sources.map(source=>estimateChannel(source,at));let byteAt=0,evmSum=0,evmCount=0,phaseSum=0,timingSum=0;
  for(let symbol=0;symbol<4;symbol++){if(!quiet)postMessage({type:'ofdm-symbol',symbol:symbol+1});const channelCodes=[];for(let ch=0;ch<2;ch++){const equalized=equalize(sources[ch],at+(symbol+1)*SYMBOL+CP,channels[ch]),re=equalized.re,im=equalized.im;const pilotBins=[24,37,51,65,79,93,107,119],expected=((index+symbol)&1)?-1:1,angles=[],magnitudes=[];for(const bin of pilotBins){angles.push(Math.atan2(im[bin]*expected,re[bin]*expected));magnitudes.push(Math.hypot(re[bin],im[bin]))}unwrap(angles);const line=fit(pilotBins,angles),gain=Math.max(median(magnitudes),1e-12);phaseSum+=line.intercept;timingSum+=-line.slope*FFT/(2*Math.PI);const codes=[];for(let carrier=0;carrier<CARRIERS;carrier++){if(PILOTS.has(carrier))continue;const bin=FIRST_BIN+carrier,phase=line.intercept+line.slope*bin,c=Math.cos(-phase),s=Math.sin(-phase),zr=(re[bin]*c-im[bin]*s)/gain,zi=(re[bin]*s+im[bin]*c)/gain,ri=nearest(zr),ii=nearest(zi);codes.push(ri|(ii<<2));evmSum+=(zr-LEVELS[ri])**2+(zi-LEVELS[ii])**2;evmCount++}channelCodes.push(codes)}for(let i=0;i<88;i++)scrambled[byteAt++]=channelCodes[0][i]|(channelCodes[1][i]<<4)}
  const inner=innerFecDecode(whiten(scrambled,index)),packet=inner?.packet||new Uint8Array(312),view=new DataView(packet.buffer),sent=view.getUint32(28,true);packet.fill(0,28,32);const computed=crc32(packet);view.setUint32(28,sent,true);const evm=Math.sqrt(evmSum/Math.max(evmCount,1)),snr=-20*Math.log10(Math.max(evm,1e-9));return {packet,valid:!!inner&&view.getUint32(0,true)===MAGIC&&view.getUint8(4)===4&&sent===computed,sent,computed,score,evm,snr,phase:phaseSum/4,timing:timingSum/4,cfo:cpCfo(at),sampleOffset:at,swap}}

function decodeRobust(at,index,score){let best=decode(at,index,score,false,false);if(best.valid)return best;for(let delta=-4;delta<=4;delta++){for(const swap of [false,true]){if(delta===0&&!swap)continue;const candidate=decode(at+delta,index,score,swap,true);if(candidate.valid)return candidate;if(candidate.evm<best.evm)best=candidate}}return best}

function decodeMono(at,index,score,quiet=false){const codes=new Uint8Array(704),channels=[estimateChannel(audioL,at),estimateChannel(audioR,at)];let codeAt=0,evmSum=0,evmCount=0,phaseSum=0,timingSum=0;for(let symbol=0;symbol<8;symbol++){if(!quiet)postMessage({type:'ofdm-symbol',symbol:symbol+1});const base=at+(symbol+1)*SYMBOL+CP,equalized=[equalize(audioL,base,channels[0]),equalize(audioR,base,channels[1])],pilotBins=[24,37,51,65,79,93,107,119],expected=((index+symbol)&1)?-1:1,angles=[],magnitudes=[];for(const bin of pilotBins){const z=mrc(equalized[0],equalized[1],channels[0],channels[1],bin);angles.push(Math.atan2(z.im*expected,z.re*expected));magnitudes.push(Math.hypot(z.re,z.im))}unwrap(angles);const line=fit(pilotBins,angles),gain=Math.max(median(magnitudes),1e-12);phaseSum+=line.intercept;timingSum+=-line.slope*FFT/(2*Math.PI);for(let carrier=0;carrier<CARRIERS;carrier++){if(PILOTS.has(carrier))continue;const bin=FIRST_BIN+carrier,z=mrc(equalized[0],equalized[1],channels[0],channels[1],bin),phase=line.intercept+line.slope*bin,c=Math.cos(-phase),s=Math.sin(-phase),zr=(z.re*c-z.im*s)/gain,zi=(z.re*s+z.im*c)/gain,ri=nearest(zr),ii=nearest(zi);codes[codeAt++]=ri|(ii<<2);evmSum+=(zr-LEVELS[ri])**2+(zi-LEVELS[ii])**2;evmCount++}}const scrambled=new Uint8Array(352);for(let i=0;i<352;i++)scrambled[i]=codes[i*2]|(codes[i*2+1]<<4);const inner=innerFecDecode(whiten(scrambled,index)),packet=inner?.packet||new Uint8Array(312),view=new DataView(packet.buffer),sent=view.getUint32(28,true);packet.fill(0,28,32);const computed=crc32(packet);view.setUint32(28,sent,true);const evm=Math.sqrt(evmSum/Math.max(evmCount,1)),snr=-20*Math.log10(Math.max(evm,1e-9));return{packet,valid:!!inner&&view.getUint32(0,true)===MAGIC&&view.getUint8(4)===4&&sent===computed,sent,computed,score,evm,snr,phase:phaseSum/8,timing:timingSum/8,cfo:cpCfo(at),sampleOffset:at,swap:false,mono:true}}
function decodeMonoRobust(at,index,score){let best=decodeMono(at,index,score,false);if(best.valid)return best;for(let delta=-16;delta<=16;delta++){const offset=at+delta;if(delta===0||offset<0||offset+MONO_PACKET_SAMPLES>audioL.length)continue;const candidate=decodeMono(offset,index,score,true);if(candidate.valid)return candidate;if(candidate.evm<best.evm)best=candidate}return best}

function accept(result,index){if(!result.valid){crcBad++;drops++;postMessage({type:'ofdm-packet',valid:false,index,crcOk,crcBad,drops,...result});return false}crcOk++;const p=result.packet,v=new DataView(p.buffer),group=v.getUint16(6,true),shard=v.getUint8(8),dataShards=v.getUint8(9),total=v.getUint32(12,true),offset=v.getUint32(16,true),size=v.getUint16(20,true),packetIndex=v.getUint16(22,true);totalSize=total;imageCrc=v.getUint32(24,true);lastPacketSeen=Math.max(lastPacketSeen,packetIndex);if(!started)started=performance.now();if(!groups.has(group))groups.set(group,{k:dataShards,shards:new Map(),parity:new Map()});const grp=groups.get(group);if(shard<dataShards)grp.shards.set(shard,p.slice(32));else if(shard>=DATA_SHARDS)grp.parity.set(shard-DATA_SHARDS,p.slice(32));postMessage({type:'ofdm-packet',valid:true,index:packetIndex,group,shard,dataShards,total,offset,size,crcOk,crcBad,drops,...result});assemble();return true}
let lastPartial=0;
function assemble(){
  if(!totalSize)return;
  const count=Math.ceil(totalSize/(DATA_SHARDS*PAYLOAD)),chunks=[];let full=true;
  for(let g=0;g<count;g++){
    const group=groups.get(g);
    if(!group||(group.shards.size<group.k&&!requestRsRecover(group,g))){full=false;break}
    let ready=true;for(let s=0;s<group.k;s++)if(!group.shards.has(s)){ready=false;break}
    if(!ready){full=false;break}
    for(let s=0;s<group.k;s++)chunks.push(group.shards.get(s));
  }
  if(!chunks.length)return;
  let bytes=0;for(const c of chunks)bytes+=c.length;
  const size=full?totalSize:Math.min(bytes,totalSize);
  if(!full&&size<lastPartial+16384)return;
  const data=new Uint8Array(size);
  let at=0;for(const chunk of chunks){const n=Math.min(chunk.length,size-at);data.set(chunk.subarray(0,n),at);at+=n;if(at>=size)break}
  if(full){
    if(lastPacketSeen+1<count*GROUP_SHARDS)return;
    if(completeSent)return;
    completeSent=true;
    const seconds=(performance.now()-started)/1000;
    postMessage({type:'ofdm-complete',valid:crc32(data)===imageCrc,data:data.buffer,total:totalSize,crc:crc32(data),expectedCrc:imageCrc,rate:totalSize*8/Math.max(seconds,1)/1000},[data.buffer]);
  }else{
    /* Contiguous prefix grew by >= one container block: let the app CRC-check
     * the newly-covered blocks so bad ones turn red during reception. */
    lastPartial=size;
    postMessage({type:'ofdm-partial',data:data.buffer,bytes:size,total:totalSize},[data.buffer]);
  }
}
function processAudio(){if(!armed)return;for(;;){const found=findSync();if(!found)return;const monoMode=decoderMode==='mono',needed=monoMode?MONO_PACKET_SAMPLES:PACKET_SAMPLES,stride=Math.ceil(needed/28)*28;if(audioL.length<found.at+needed+4)return;let result=monoMode?decodeMonoRobust(found.at,expectedPacket,found.score):decodeRobust(found.at,expectedPacket,found.score);if(!result.valid){for(let skip=1;skip<=PARITY_SHARDS;skip++){const candidate=monoMode?decodeMonoRobust(found.at,expectedPacket+skip,found.score):decodeRobust(found.at,expectedPacket+skip,found.score);if(candidate.valid){expectedPacket=candidate.wireIndex??expectedPacket+skip;result=candidate;break}}}if(result.valid&&Number.isInteger(result.wireIndex))expectedPacket=result.wireIndex;if(!result.valid&&expectedPacket>PARITY_SHARDS){for(let restart=0;restart<=PARITY_SHARDS;restart++){const candidate=monoMode?decodeMonoRobust(found.at,restart,found.score):decodeRobust(found.at,restart,found.score);if(candidate.valid){expectedPacket=candidate.wireIndex??restart;result=candidate;groups.clear();failedGroups.clear();totalSize=0;imageCrc=0;postMessage({type:'ofdm-restart',index:expectedPacket});break}}}if(!result.valid&&found.score<.6){const skip=found.at+64;audioL.splice(0,skip);audioR.splice(0,skip);locked=false;continue}postMessage({type:'ofdm-sync',correlation:found.score,index:expectedPacket});accept(result,expectedPacket);locked=found.score>=.6;let consume=needed;if(stride>needed&&audioL.length>=found.at+stride+4){let paddingPeak=0;for(let i=needed;i<stride;i++)paddingPeak=Math.max(paddingPeak,Math.abs(audioL[found.at+i]),Math.abs(audioR[found.at+i]));if(paddingPeak<.02)consume=stride}audioL.splice(0,found.at+consume);audioR.splice(0,found.at+consume);lastPacketSeen=expectedPacket++;if(totalSize&&lastPacketSeen+1>=Math.ceil(totalSize/(DATA_SHARDS*PAYLOAD))*GROUP_SHARDS)finalizeRecovery()}}
function append(left,right,rate){
  inputRate=rate||inputRate;
  const before=audioL.length;
  if(Math.abs(inputRate-RATE)<0.5){
    for(let i=0;i<left.length;i++){audioL.push(left[i]);audioR.push(right[i]??left[i])}
  }else{
    const step=inputRate/RATE,cutoff=Math.min(1,RATE/inputRate);
    for(let i=0;i<left.length;i++){rsL.push(left[i]);rsR.push(right[i]??left[i])}
    const avail=rsBase+rsL.length;
    while(rsPos+RS_HALF<avail){
      const c0=Math.floor(rsPos),frac=rsPos-c0;
      let accL=0,accR=0,norm=0;
      for(let t=-RS_HALF+1;t<=RS_HALF;t++){
        const idx=c0+t-rsBase;
        if(idx<0||idx>=rsL.length)continue;
        const k=winSinc(t-frac,cutoff);
        accL+=rsL[idx]*k;accR+=rsR[idx]*k;norm+=k;
      }
      norm=norm||1;
      audioL.push(accL/norm);audioR.push(accR/norm);
      rsPos+=step;
    }
    const keep=Math.floor(rsPos)-RS_HALF-1;
    if(keep>rsBase){const d=keep-rsBase;rsL.splice(0,d);rsR.splice(0,d);rsBase+=d}
  }
  receivedSamples+=audioL.length-before;
  detectCarrier();processAudio();
}
function receivePcm(m){if(!armed){preRoll.push(m);if(preRoll.length>PRE_ROLL_CHUNKS)preRoll.shift();return}const l=new Float32Array(m.left),r=new Float32Array(m.right);if(W)appendWasm(l,r,m.rate);else append(l,r,m.rate)}
onmessage=event=>{const m=event.data;if(m.type==='connect-pcm'){pcmPort=event.ports?.[0]||null;if(pcmPort){pcmPort.onmessage=e=>receivePcm(e.data);pcmPort.start?.()}}else if(m.type==='arm'){armed=true;fecEpoch++;currentBlock=m.block||0;decoderMode='stereo';started=0;audioL=[];audioR=[];rsL=[];rsR=[];rsBase=0;rsPos=RS_HALF;preRoll=[];expectedPacket=m.index||0;crcOk=crcBad=drops=reportAt=receivedSamples=0;carrierReported=locked=false;lastPartial=0;lastPacketSeen=-1;completeSent=false;groups.clear();failedGroups.clear();if(W){W.sxd_set_mode(0,0);W.sxd_reset(expectedPacket)}postMessage({type:'ofdm-armed',engine:W?'wasm':'js-fallback'})}else if(m.type==='boundary'){armed=false;audioL=[];audioR=[];rsL=[];rsR=[];carrierReported=locked=false;finalizeRecovery()}else if(m.type==='disarm'){armed=false;fecEpoch++;audioL=[];audioR=[];rsL=[];rsR=[];groups.clear();failedGroups.clear();carrierReported=locked=false;completeSent=false}else if(m.type==='pcm')receivePcm(m)};
