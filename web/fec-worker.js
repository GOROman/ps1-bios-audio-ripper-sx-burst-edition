const PAYLOAD=280;
const gfExp=new Uint8Array(512),gfLog=new Uint8Array(256);
{let v=1;for(let i=0;i<255;i++){gfExp[i]=v;gfLog[v]=i;v<<=1;if(v&0x100)v^=0x11d}for(let i=255;i<512;i++)gfExp[i]=gfExp[i-255]}
const gfMul=(a,b)=>(a&&b)?gfExp[gfLog[a]+gfLog[b]]:0;
const gfInv=a=>gfExp[255-gfLog[a]];

function solve(B,S,e){
  for(let col=0;col<e;col++){
    let pivot=col;while(pivot<e&&!B[pivot][col])pivot++;
    if(pivot===e)return false;
    if(pivot!==col){[B[pivot],B[col]]=[B[col],B[pivot]];[S[pivot],S[col]]=[S[col],S[pivot]]}
    const inverse=gfInv(B[col][col]);
    for(let j=col;j<e;j++)B[col][j]=gfMul(B[col][j],inverse);
    for(let i=0;i<PAYLOAD;i++)S[col][i]=gfMul(S[col][i],inverse);
    for(let row=0;row<e;row++){
      if(row===col||!B[row][col])continue;
      const factor=B[row][col];
      for(let j=col;j<e;j++)B[row][j]^=gfMul(factor,B[col][j]);
      for(let i=0;i<PAYLOAD;i++)S[row][i]^=gfMul(factor,S[col][i]);
    }
  }
  return true;
}

onmessage=event=>{
  const {epoch,group,k,shards,parity}=event.data,known=new Map(shards),par=new Map(parity),missing=[];
  for(let index=0;index<k;index++)if(!known.has(index))missing.push(index);
  const count=missing.length;
  if(!count||par.size<count){postMessage({epoch,group,ok:count===0,recovered:0,shards:[]});return}
  const rows=[...par.keys()].sort((a,b)=>a-b).slice(0,count),matrix=[],values=[];
  for(let at=0;at<count;at++){
    const row=rows[at],value=Uint8Array.from(par.get(row));
    for(let index=0;index<k;index++){const shard=known.get(index);if(!shard)continue;const coefficient=gfInv(row^(6+index));for(let byte=0;byte<PAYLOAD;byte++)value[byte]^=gfMul(coefficient,shard[byte])}
    const coefficients=new Uint8Array(count);for(let index=0;index<count;index++)coefficients[index]=gfInv(row^(6+missing[index]));
    matrix.push(coefficients);values.push(value);
  }
  const ok=solve(matrix,values,count),recovered=ok?values.map((value,index)=>[missing[index],value]):[];
  postMessage({epoch,group,ok,recovered:ok?count:0,shards:recovered},ok?values.map(value=>value.buffer):[]);
};
postMessage({type:'ready'});
