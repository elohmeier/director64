// Source-free end-to-end proof of actual browser glyph/PCM capture. It uses
// synthetic stage pixels and an oscillator, never converted retail assets.
import {chromium} from "playwright";
import {createServer} from "node:http";
import {readFileSync, writeFileSync, mkdirSync, copyFileSync} from "node:fs";
import {resolve, join} from "node:path";
import {run} from "./browser.mjs";
const output = resolve(process.argv[2] ?? "build/validation/browser/selftest");
mkdirSync(output, {recursive:true});
const html = `<!doctype html><button id="start">Start</button><script type="module">
import {attachValidation} from '/validation.js';
const width=64,height=32,heap=new Uint8Array(width*height*4),canvas=document.createElement('canvas');
canvas.width=width;canvas.height=height;
const g=canvas.getContext('2d');let tick=0;
const face=await new FontFace('SyntheticTest','url(/font.ttf)').load();document.fonts.add(face);
function render(){g.fillStyle='#204060';g.fillRect(0,0,width,height);g.fillStyle='white';g.font='20px SyntheticTest';g.fillText('FONT',2,22);g.fillStyle='#ff8040';g.fillRect(45+tick%10,3,4,4);heap.set(g.getImageData(0,0,width,height).data);return 1;}
const context=new AudioContext(),master=context.createGain(),osc=context.createOscillator();
master.gain.value=.25;master.connect(context.destination);osc.frequency.value=440;osc.connect(master);osc.start();
const module={HEAPU8:heap,_d64_pixels:()=>0,_d64_render:render,_d64_probe_render(){},ccall(n,r,t,a){if(a[0].startsWith('step'))tick++;return JSON.stringify({movie:'SYNTHETIC',frame:tick,error:'',script_errors:0,quit:false});}};
const audio={context,master,errors:[],resume:()=>context.resume(),suspend:()=>context.suspend(),stopAll(){}};
window.api=attachValidation({module,audio,videos:{errors:[],closeAll(){}},present(){},width,height,seed:42});
window.master=master;window.ready=true;
document.querySelector('#start').onclick=()=>{window.director64={validation:api,module,audio};};
</script>`;
const server = createServer((request,response)=>{
  if(request.url==='/validation.js'){response.setHeader('Content-Type','text/javascript');response.end(readFileSync('platforms/web/site/validation.js'));}
  else if(request.url==='/font.ttf'){response.setHeader('Content-Type','font/ttf');response.end(readFileSync('third_party/fonts/monogram.ttf'));}
  else {response.setHeader('Content-Type','text/html');response.end(html);}
});
let browser;
try {
  await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
  browser=await chromium.launch({args:['--autoplay-policy=no-user-gesture-required']});
  const page=await browser.newPage();
  await page.goto(`http://127.0.0.1:${server.address().port}`);
  await page.waitForFunction(()=>window.ready);
  const frames=[];
  for(let i=0;i<4;i++){
    const frame=await page.evaluate(()=>api.capture());
    const rgba=Buffer.from(frame.rgba,'base64');
    let glyph=0;
    for(let at=0;at<rgba.length;at+=4)if(rgba[at]===255&&rgba[at+1]===255&&rgba[at+2]===255)glyph++;
    if(glyph<5)throw new Error('actual browser glyph coverage is absent');
    writeFileSync(join(output,`frame-${i}.rgba`),Buffer.from(frame.rgba,'base64'));
    writeFileSync(join(output,`frame-${i}.png`),Buffer.from(frame.png,'base64'));
    frames.push({rgba:`frame-${i}.rgba`,png:`frame-${i}.png`,state:frame.state});
    await page.evaluate(()=>api.advance('step 1 0 0 0',true));
  }
  async function audio(name,mute){
    await page.evaluate(async mute=>{master.gain.value=mute?0:.25;await new Promise(resolve=>setTimeout(resolve,100));await api.startAudio();},mute);
    await page.waitForTimeout(350);
    const result=await page.evaluate(()=>api.takeAudio());
    const bytes=Buffer.from(result.pcm,'base64');
    if(!bytes.length)throw new Error('audio tap recorded no samples');
    writeFileSync(join(output,`${name}.f32`),bytes);
    return {file:`${name}.f32`,rate:result.rate,channels:result.channels};
  }
  const audible=await audio('audible',false),muted=await audio('muted',true);
  const report={schema_version:1,status:'captured',stage:[64,32],environment:{browser:browser.version()},
    clock:'synthetic-controlled-60hz',provenance:{source:'synthetic-generated',font:'CC0 monogram'},
    checkpoints:[{label:'synthetic-glyph-motion',frames,observation:{sequence:{min_changed_frames:3,max_blank_frames:0,max_identical_run:1}},audio:audible}],muted_audio:muted};
  writeFileSync(join(output,'capture.json'),JSON.stringify(report,null,2)+'\n');
  process.stdout.write(JSON.stringify({status:'captured',frames:frames.length,audible,muted})+'\n');
}finally{
  await browser?.close();
  await new Promise(resolve=>server.close(resolve));
}
// Exercise the entire runner too: observe inside an existing input budget,
// encode/transport with AudioContext held, continue realtime service, finalize
// decoders, and capture another audible window. No authored ticks may be added.
const site=join(output,'site');mkdirSync(site,{recursive:true});
writeFileSync(join(site,'index.html'),html);
copyFileSync('platforms/web/site/validation.js',join(site,'validation.js'));
copyFileSync('third_party/fonts/monogram.ttf',join(site,'font.ttf'));
const replay={steps:[{idle:120,expect:{frame:120},label:'after-window',
  observe:{label:'inside-input',sequence:{frames:7,stride_ticks:5,min_changed_frames:6},audio:{seconds:.4,min_rms:.01}}},
  {idle:60,expect:{frame:180},label:'continued'}],observations:{continued:{sequence:{frames:7,stride_ticks:5,min_changed_frames:6},audio:{seconds:.5,min_rms:.01}}}};
const replayed=await run(site,replay,join(output,'runner'),{fast:false});
if(replayed.status!=='captured'||replayed.last_state.frame!==210)
  throw new Error(`synthetic realtime runner failed: ${replayed.error??JSON.stringify(replayed.last_state)}`);
process.stdout.write('full browser runner preserved ticks and continued after artifact holds\n');
