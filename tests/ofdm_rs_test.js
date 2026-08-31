// Exercise the worker's RS(16+6) erasure decoder against the transmitter's
// parity formula in src/ofdm_packet.c.  Loads web/ofdm-worker.js in a vm and
// appends an export shim so the in-scope gf*/rsRecover helpers are reachable.
const fs=require('node:fs'),vm=require('node:vm'),assert=require('node:assert');
const source=fs.readFileSync('web/ofdm-worker.js').toString()
  +'\n;globalThis.__rs={gfMul,gfInv,rsRecover,PAYLOAD};';
const context={console,performance:{now:()=>0},Float32Array,Float64Array,Uint8Array,DataView,Map,Set,Math,postMessage:()=>{}};
vm.createContext(context);vm.runInContext(source,context);
const {gfMul,gfInv,rsRecover,PAYLOAD}=context.__rs;
assert.equal(PAYLOAD,280,'PAYLOAD mismatch');

// GF(256) sanity: 1/x * x == 1
for(let x=1;x<256;x++)assert.equal(gfMul(gfInv(x),x),1,`gfInv(${x}) wrong`);

// Transmitter parity: parity[r][i] = XOR_c A(r,c) * data[c][i], A(r,c)=1/(r^(6+c))
function encodeParity(data,k){
  const parity=[];
  for(let r=0;r<6;r++){
    const row=new Uint8Array(PAYLOAD);
    for(let c=0;c<k;c++){const coef=gfInv(r^(6+c));for(let i=0;i<PAYLOAD;i++)row[i]^=gfMul(coef,data[c][i])}
    parity.push(row);
  }
  return parity;
}
function rng(seed){return()=>{seed=(seed*1103515245+12345)&0x7fffffff;return seed>>>16&255}}

function trial(k,dropData,dropParity){
  const r=rng(1234+k*97+dropData.join('')*7+dropParity.join('')*13);
  const data=[];for(let c=0;c<k;c++){const s=new Uint8Array(PAYLOAD);for(let i=0;i<PAYLOAD;i++)s[i]=r();data.push(s)}
  const parity=encodeParity(data,k);
  const group={k,shards:new Map(),parity:new Map()};
  for(let c=0;c<k;c++)if(!dropData.includes(c))group.shards.set(c,Uint8Array.from(data[c]));
  for(let p=0;p<6;p++)if(!dropParity.includes(p))group.parity.set(p,Uint8Array.from(parity[p]));
  const ok=rsRecover(group,0);
  const enough=(6-dropParity.length)>=dropData.length;
  assert.equal(ok,enough,`k=${k} drop ${dropData} parity ${dropParity}: rsRecover=${ok} expected ${enough}`);
  if(!enough)return;
  for(let c=0;c<k;c++)assert.deepEqual([...group.shards.get(c)],[...data[c]],`k=${k} shard ${c} not recovered (dropped ${dropData})`);
}

// full group, 1..6 data shards lost, exactly enough parity
trial(16,[3],[0]);
trial(16,[0,15],[1,2]);
trial(16,[2,7,11],[0,1,3]);
trial(16,[1,4,9,14],[0,1,2,3]);
trial(16,[0,3,6,9,12],[0,1,2,3,4]);
trial(16,[0,2,5,8,11,15],[0,1,2,3,4,5]);
// spread parity choice (decoder must pick the received rows)
trial(16,[5,6],[0,3]);
// last-group short k
trial(9,[2,5],[1,2]);
trial(1,[0],[0]);
// not enough parity -> must fail cleanly, no throw
trial(16,[0,1,2],[0,1]);
trial(16,[4],[]);
// no loss -> trivially ok
trial(32,[],[]);

console.log('PASS ofdm_rs (erasure decode 1..6 shards, GF(256) inverse table)');
