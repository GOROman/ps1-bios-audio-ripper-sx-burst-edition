#!/usr/bin/env python3
"""Canonical SX stereo OFDM modem and loss-recovery reference."""
import argparse,binascii,math,struct,wave
from pathlib import Path
import numpy as np

RATE=44100;FFT=512;CP=64;BINS=np.arange(24,220)
PILOTS=np.array([0,65,130,195]);DATA=np.array([i for i in range(196) if i not in set(PILOTS)])
PACKET_BYTES=768;HEADER=struct.Struct('<IBBHBBBBIIHHII');PAYLOAD=PACKET_BYTES-HEADER.size
MAGIC=0x314F5853;K=16;P=4;SYMBOLS=5

def crc32(data):return binascii.crc32(data)&0xffffffff
def gf_tables():
 e=[0]*512;l=[0]*256;x=1
 for i in range(255):e[i]=x;l[x]=i;x<<=1;x^=0x11d if x&0x100 else 0
 for i in range(255,512):e[i]=e[i-255]
 return e,l
GE,GL=gf_tables()
def gm(a,b):return 0 if not a or not b else GE[GL[a]+GL[b]]
def rows(k):return [[GE[255-GL[r^(P+c)]] for c in range(k)] for r in range(P)]
def parity(shards):
 out=[bytearray(PAYLOAD) for _ in range(P)]
 for r,row in enumerate(rows(len(shards))):
  for c,src in enumerate(shards):
   q=row[c]
   for i,v in enumerate(src):out[r][i]^=gm(q,v)
 return [bytes(x) for x in out]
def inv(matrix):
 n=len(matrix);m=[row[:]+[int(i==j) for j in range(n)] for i,row in enumerate(matrix)]
 for col in range(n):
  pivot=next((r for r in range(col,n) if m[r][col]),None)
  if pivot is None:raise ValueError('singular FEC matrix')
  m[col],m[pivot]=m[pivot],m[col];q=GE[255-GL[m[col][col]]];m[col]=[gm(x,q) for x in m[col]]
  for r in range(n):
   if r!=col and m[r][col]:q=m[r][col];m[r]=[x^gm(q,y) for x,y in zip(m[r],m[col])]
 return [row[n:] for row in m]
def recover(shards,k):
 selected=sorted(shards)[:k];pr=rows(k);matrix=[]
 for i in selected:matrix.append([int(c==i) for c in range(k)] if i<k else pr[i-K])
 back=inv(matrix);src=[shards[i] for i in selected];out=[bytearray(PAYLOAD) for _ in range(k)]
 for r,row in enumerate(back):
  for c,block in enumerate(src):
   q=row[c]
   for i,v in enumerate(block):out[r][i]^=gm(q,v)
 return [bytes(x) for x in out]
def packetize(data):
 image_crc=crc32(data);pieces=[data[i:i+PAYLOAD].ljust(PAYLOAD,b'\0') for i in range(0,len(data),PAYLOAD)];packets=[]
 for group,start in enumerate(range(0,len(pieces),K)):
  source=pieces[start:start+K];all_shards=source+[bytes(PAYLOAD)]*(K-len(source))+parity(source)
  for shard,payload in enumerate(all_shards):
   flags=int(shard>=K);offset=(start+shard)*PAYLOAD if not flags else 0xffffffff
   payload_size=min(PAYLOAD,max(0,len(data)-offset)) if not flags and shard<len(source) else (PAYLOAD if flags else 0)
   index=len(packets);head=HEADER.pack(MAGIC,1,flags,group,shard,len(source),P,0,len(data),offset,payload_size,index,image_crc,0)
   check=crc32(head+payload);packets.append(HEADER.pack(MAGIC,1,flags,group,shard,len(source),P,0,len(data),offset,payload_size,index,image_crc,check)+payload)
 return packets
def depacketize(packets):
 groups={};total=image_crc=None
 for packet in packets:
  if len(packet)!=PACKET_BYTES:continue
  f=list(HEADER.unpack(packet[:HEADER.size]));sent=f[-1];f[-1]=0
  if f[0]!=MAGIC or crc32(HEADER.pack(*f)+packet[HEADER.size:])!=sent:continue
  group,shard,k=f[3],f[4],f[5];total=f[8];image_crc=f[12]
  if shard<k or shard>=K:groups.setdefault(group,(k,{}))[1][shard]=packet[HEADER.size:]
 result=[]
 for group in range(max(groups)+1):
  k,shards=groups[group]
  if len(shards)<k:raise ValueError(f'group {group}: {len(shards)}/{k}')
  result.extend(recover(shards,k))
 data=b''.join(result)[:total]
 if crc32(data)!=image_crc:raise ValueError('image CRC mismatch')
 return data
def whiten(data,index):
 state=(0x9e3779b9^index)&0xffffffff;out=bytearray(len(data))
 for i,v in enumerate(data):state^=(state<<13)&0xffffffff;state^=state>>17;state^=(state<<5)&0xffffffff;state&=0xffffffff;out[i]=v^(state&255)
 return bytes(out)
def qam(n):
 levels=np.array([-3,-1,3,1],float)/math.sqrt(10);return levels[n&3]+1j*levels[(n>>2)&3]
def unqam(z):
 levels=np.array([-3,-1,3,1],float)/math.sqrt(10);a=np.argmin(abs(z.real[:,None]-levels),axis=1);b=np.argmin(abs(z.imag[:,None]-levels),axis=1);return (a|(b<<2)).astype(np.uint8)
def nibbles(data):
 a=np.frombuffer(data,dtype=np.uint8);n=np.empty(len(a)*2,np.uint8);n[::2]=a&15;n[1::2]=a>>4;return n
def sync_symbol():
 freq=np.zeros((2,FFT),complex);pattern=np.where(((BINS*73+19)&1)==0,1.,-1.)
 for ch in range(2):freq[ch,BINS]=pattern;freq[ch,FFT-BINS]=pattern
 td=np.fft.ifft(freq,axis=1).real*4;return np.concatenate((td[:,-CP:],td),axis=1).T
def modulate(packet,index):
 nib=nibbles(whiten(packet,index));out=[sync_symbol()]
 for symbol in range(4):
  freq=np.zeros((2,FFT),complex);part=nib[symbol*len(DATA)*2:(symbol+1)*len(DATA)*2]
  for ch in range(2):
   freq[ch,BINS[DATA]]=qam(part[ch::2]);pilot=1. if ((index+symbol)&1)==0 else -1.;freq[ch,BINS[PILOTS]]=pilot;freq[ch,FFT-BINS]=np.conj(freq[ch,BINS])
  td=np.fft.ifft(freq,axis=1).real*4;out.append(np.concatenate((td[:,-CP:],td),axis=1).T)
 return np.concatenate(out)
def demodulate(audio,index):
 result=[]
 for symbol in range(4):
  block=audio[(symbol+1)*(FFT+CP):(symbol+2)*(FFT+CP)];spec=np.fft.fft(block[CP:CP+FFT],axis=0);parts=[]
  for ch in range(2):
   expected=1. if ((index+symbol)&1)==0 else -1.;pil=spec[BINS[PILOTS],ch]*expected;phase=np.unwrap(np.angle(pil));fit=np.polyfit(BINS[PILOTS],phase,1);gain=np.median(abs(pil));z=spec[BINS[DATA],ch]*np.exp(-1j*np.polyval(fit,BINS[DATA]))/max(gain,1e-9);parts.append(unqam(z))
  mixed=np.empty(len(DATA)*2,np.uint8);mixed[::2]=parts[0];mixed[1::2]=parts[1];result.append(mixed)
 n=np.concatenate(result);raw=bytes((n[::2]|(n[1::2]<<4)).tolist());return whiten(raw,index)
def selftest():
 rng=np.random.default_rng(1);data=rng.integers(0,256,339932,np.uint8).tobytes();packets=packetize(data)
 decoded=[demodulate(modulate(packet,i),i) for i,packet in enumerate(packets)]
 kept=[p for i,p in enumerate(decoded) if HEADER.unpack(p[:HEADER.size])[4] not in (2,17)]
 assert depacketize(kept)==data
 seconds=len(packets)*SYMBOLS*(FFT+CP)/RATE
 print(f'PASS OFDM bytes={len(data)} packets={len(packets)} airtime={seconds:.2f}s crc32={crc32(data):08x}')
def main():
 ap=argparse.ArgumentParser();sub=ap.add_subparsers(dest='cmd',required=True);sub.add_parser('selftest');enc=sub.add_parser('encode');enc.add_argument('input',type=Path);enc.add_argument('output',type=Path);a=ap.parse_args()
 if a.cmd=='selftest':selftest();return
 data=a.input.read_bytes();audio=np.concatenate([modulate(p,i) for i,p in enumerate(packetize(data))]);pcm=(np.clip(audio,-.98,.98)*32767).astype('<i2')
 with wave.open(str(a.output),'wb') as w:w.setparams((2,2,RATE,len(pcm),'NONE','not compressed'));w.writeframes(pcm.tobytes())
 print(f'wrote {a.output}: {len(audio)/RATE:.2f}s')
if __name__=='__main__':main()
