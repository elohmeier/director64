// Actual Chromium Web Audio graph, including multi-voice summing and the
// production final output clamp. Generated PCM only; no game media.
import {chromium} from "playwright";
import {createServer} from "node:http";
import {readFileSync, writeFileSync, mkdirSync} from "node:fs";
import {resolve, join} from "node:path";
const output=resolve(process.argv[2]??"build/validation/browser/audio-output");
mkdirSync(output,{recursive:true});
const server=createServer((request,response)=>{
  const name=new URL(request.url,"http://localhost").pathname;
  if(["/audio.js","/flash-audio.js","/validation.js"].includes(name)) {
    response.setHeader("Content-Type","text/javascript");
    response.end(readFileSync("platforms/web/site"+name));
  } else response.end("<!doctype html><title>Generated PCM output test</title>");
});
let browser;
try {
  await new Promise(resolve=>server.listen(0,"127.0.0.1",resolve));
  browser=await chromium.launch({args:["--autoplay-policy=no-user-gesture-required"]});
  const page=await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}`);
  const results=await page.evaluate(async()=>{
    const {AudioEngine}=await import("/audio.js");
    const NativeAudioContext=window.AudioContext;
    const patterns=[-2,-1.01561,-1,-.5,0,.125,.5,1,1.01561,2];
    const output=[];
    for(const [name,second,volume,muted] of [
      ["decoded-overshoot",0,1,false],["two-voice-sum",.6,1,false],
      ["master-volume",.6,.25,false],["muted",.6,1,true]]) {
      const length=patterns.length*128,context=new OfflineAudioContext(2,length,48000);
      window.AudioContext=function(){return context;};
      const engine=new AudioEngine(()=>null,2);
      engine.setVolume(volume,muted);
      const buffers=[];
      for(let voice=0;voice<(second?2:1);voice++) {
        const buffer=context.createBuffer(2,length,48000);
        for(let channel=0;channel<2;channel++) {
          const samples=buffer.getChannelData(channel);
          for(let i=0;i<length;i++)samples[i]=(voice?second:patterns[Math.floor(i/128)])*(channel?-1:1);
        }
        buffers.push(buffer);
      }
      engine.load=name=>({buffer:buffers[Number(name)],rate:48000,frames:length});
      engine.play(0,"0",false,0,0);if(second)engine.play(1,"1",false,0,0);
      const rendered=await context.startRendering();
      let maxError=0,peak=0,clipped=0;
      const samples=[];
      for(let i=0;i<length;i++)for(let c=0;c<2;c++) {
        const actual=rendered.getChannelData(c)[i];
        const input=(patterns[Math.floor(i/128)]+second)*(c?-1:1)*(muted?0:volume);
        const expected=Math.max(-1,Math.min(1,input));
        maxError=Math.max(maxError,Math.abs(actual-expected));peak=Math.max(peak,Math.abs(actual));
        clipped+=Math.abs(actual)===1?1:0;samples.push(actual);
      }
      if(maxError>2e-6||peak>1)throw new Error(`${name}: final PCM error=${maxError}, peak=${peak}`);
      output.push({name,rate:48000,channels:2,frames:length,peak,max_error:maxError,clipped,samples});
    }
    // The real-time validation tap must observe the same final node, rather
    // than the pre-clamp master bus that previously reported >1 decoded PCM.
    window.AudioContext=NativeAudioContext;
    const {attachValidation}=await import("/validation.js");
    const engine=new AudioEngine(()=>null,2),context=engine.context;
    const buffer=context.createBuffer(1,context.sampleRate,context.sampleRate);
    buffer.getChannelData(0).fill(.75);
    engine.load=()=>({buffer,rate:context.sampleRate,frames:buffer.length});
    const module={_d64_probe_render(){},HEAPU8:new Uint8Array(4),_d64_pixels:()=>0};
    const api=attachValidation({module,audio:engine,videos:{errors:[]},present(){},width:1,height:1,seed:42});
    await engine.resume();engine.play(0,"a",true,0,buffer.length);engine.play(1,"b",true,0,buffer.length);
    await new Promise(resolve=>setTimeout(resolve,100));await api.startAudio();
    await new Promise(resolve=>setTimeout(resolve,200));
    const captured=api.takeAudio(),binary=atob(captured.pcm);
    const raw=Uint8Array.from(binary,c=>c.charCodeAt(0));
    const pcm=new Float32Array(raw.buffer);
    if(pcm.length<2048||pcm.some(value=>Math.abs(value)>1)||!pcm.some(value=>value===1))
      throw new Error("actual validation tap did not capture bounded final PCM");
    output.push({name:"realtime-final-tap",rate:captured.rate,channels:2,frames:pcm.length/2,
      peak:Math.max(...pcm),master_gain:captured.master_gain,samples:[...pcm]});
    engine.stopAll();await context.close();
    return output;
  });
  for(const result of results) {
    writeFileSync(join(output,result.name+".f32"),Buffer.from(new Float32Array(result.samples).buffer));
    delete result.samples;
  }
  writeFileSync(join(output,"report.json"),JSON.stringify({status:"passed",browser:browser.version(),
    source:"generated-float-PCM",scope:"production Web Audio graph in OfflineAudioContext",results},null,2)+"\n");
  process.stdout.write("browser final PCM: exact identity below full scale, stereo hard clip after multi-voice/master gain, mute passed\n");
} finally {
  await browser?.close();await new Promise(resolve=>server.close(resolve));
}
