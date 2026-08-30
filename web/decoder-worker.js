/* Burst Edition wire V6 has no FSK phase. This real-time thread forwards
 * stereo PCM to the OFDM worker and emits lightweight level/scope telemetry. */
class SxInputProcessor extends AudioWorkletProcessor {
  constructor(){super();this.pcmPort=null;this.analyzerEnabled=true;this.decimate=0;this.clipTotal=0;this.snrEstimate=0;this.pcmL=new Float32Array(2048);this.pcmR=new Float32Array(2048);this.pcmAt=0;this.vizL=new Float32Array(512);this.vizR=new Float32Array(512);this.vizAt=0;this.port.onmessage=event=>{if(event.data?.type==='connect-ofdm'){this.pcmPort=event.ports?.[0]||null;this.pcmPort?.start?.()}else if(event.data?.type==='analyzer-enabled')this.analyzerEnabled=event.data.enabled!==false}}
  snapshot(source){const out=new Float32Array(source.length);for(let i=0;i<source.length;i++)out[i]=source[(this.vizAt+i)%source.length];return out}
  tonePower(samples,frequency){let re=0,im=0;const step=2*Math.PI*frequency/sampleRate;for(let i=0;i<samples.length;i++){re+=samples[i]*Math.cos(step*i);im-=samples[i]*Math.sin(step*i)}return 2*(re*re+im*im)/(samples.length*samples.length)}
  process(inputs){
    const input=inputs[0];if(!input?.length)return true;const left=input[0],right=input[1],visualRight=right||left;
    for(let i=0;i<left.length;i++){const rv=visualRight[i]||0;this.vizL[this.vizAt]=left[i];this.vizR[this.vizAt]=rv;this.vizAt=(this.vizAt+1)&511;this.pcmL[this.pcmAt]=left[i];this.pcmR[this.pcmAt]=rv;if(++this.pcmAt===this.pcmL.length){const l=this.pcmL,r=this.pcmR,target=this.pcmPort||this.port;target.postMessage({type:'pcm',rate:sampleRate,left:l.buffer,right:r.buffer},[l.buffer,r.buffer]);this.pcmL=new Float32Array(2048);this.pcmR=new Float32Array(2048);this.pcmAt=0}}
    let sumL=0,sumR=0,sumLR=0,meanL=0,meanR=0,peakL=0,peakR=0,clips=0;
    for(let i=0;i<left.length;i++){const lv=left[i],rv=visualRight[i]||0,l=Math.abs(lv),r=Math.abs(rv);sumL+=lv*lv;sumR+=rv*rv;sumLR+=lv*rv;meanL+=lv;meanR+=rv;if(l>peakL)peakL=l;if(r>peakR)peakR=r;if(l>=.988)clips++;if(r>=.988)clips++}
    const rmsL=Math.sqrt(sumL/Math.max(left.length,1)),rmsR=Math.sqrt(sumR/Math.max(visualRight.length,1));this.clipTotal+=clips;const frame=this.decimate++;
    if(frame%12===0){const low=this.tonePower(left,3000),high=this.tonePower(left,9000),signal=Math.max(low,high),total=sumL/Math.max(left.length,1),noise=Math.max(total-signal,1e-12),instantSnr=10*Math.log10(Math.max(signal,1e-12)/noise);this.snrEstimate=this.snrEstimate*.8+instantSnr*.2;const message={type:'level',channels:input.length,rms:(rmsL+rmsR)*.5,peak:Math.max(peakL,peakR),rmsL,rmsR,peakL,peakR,dcL:meanL/left.length,dcR:meanR/visualRight.length,crestL:20*Math.log10(Math.max(peakL,1e-9)/Math.max(rmsL,1e-9)),crestR:20*Math.log10(Math.max(peakR,1e-9)/Math.max(rmsR,1e-9)),correlation:sumLR/Math.max(Math.sqrt(sumL*sumR),1e-12),balance:20*Math.log10(Math.max(rmsL,1e-9)/Math.max(rmsR,1e-9)),snr:this.snrEstimate,tone:high>low?9000:3000,toneRatio:10*Math.log10(Math.max(high,1e-12)/Math.max(low,1e-12)),clips:this.clipTotal};if(frame%36===0){message.left=this.snapshot(this.vizL);message.right=this.snapshot(this.vizR)}this.port.postMessage(message)}
    return true;
  }
}
registerProcessor('sx-input',SxInputProcessor);
