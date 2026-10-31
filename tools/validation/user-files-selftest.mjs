// Synthetic browser document flows through production UserFiles UI and bridge.
// Downloads/captures stay ignored; the print hook observes a native print call
// and afterprint lifetime, not physical printer output.
import {chromium} from 'playwright';
import {createServer} from 'node:http';
import {readFileSync,writeFileSync,mkdirSync} from 'node:fs';
import {join,resolve} from 'node:path';
import assert from 'node:assert/strict';
const output=resolve(process.argv[2]??'build/validation/browser/user-files-selftest');mkdirSync(output,{recursive:true});
const html='<!doctype html><html><head><meta charset="utf-8"><link rel="stylesheet" href="style.css"><script type="module" src="test.js"></script></head><body><main id="stage"></main></body></html>';
const source=`import {UserFiles,windows1252} from '/user-files.js';
const heap=new Uint8Array(64*1024*1024),holds=[],replies=[],completed=[],cancelled=[],freed=[];
let request={id:0,kind:0},cursor=1024*1024;
function str(at,text){const b=new TextEncoder().encode(text);heap.set(b,at);heap[at+b.length]=0;return at;}
const module={HEAPU8:heap,_malloc(n){const at=cursor;cursor+=n+8;return at;},_free(at){freed.push(at);},
_d64_user_request_id:()=>request.kind?request.id:0,_d64_user_request_kind:()=>request.kind,
_d64_user_title:()=>str(128,request.title??'Synthetic file'),_d64_user_name:()=>str(384,request.name??'test.car'),
_d64_user_filter:()=>str(640,request.filter??'*.CAR'),_d64_user_size:()=>request.data?.length??0,
_d64_user_data(){if(request.data)heap.set(request.data,4096);return 4096;},
_d64_user_reply(id,name,raw,size,pixels,width,height){let end=name;while(heap[end])end++;
replies.push({id,name:new TextDecoder('utf-8',{fatal:true}).decode(heap.slice(name,end)),raw:Array.from(heap.slice(raw,raw+size)),
rgba:pixels?Array.from(heap.slice(pixels,pixels+width*height*4)):[],width,height});request.kind=0;return 1;},
_d64_user_cancel(id){cancelled.push(id);request.kind=0;return 1;},_d64_user_complete(id,ok){completed.push([id,ok]);request.kind=0;return 1;}};
const files=new UserFiles({module,hold:(reason,on)=>holds.push([reason,on]),root:document.getElementById('stage')});
window.test={files,holds,replies,completed,cancelled,freed,set(value){request={...value,data:value.data?Uint8Array.from(value.data):null};files.poll();},dispose:()=>files.dispose()};
window.ready=true;`;
const server=createServer((request,response)=>{
 const path=new URL(request.url,'http://localhost').pathname;
 if(path==='/'){response.setHeader('Content-Type','text/html');response.end(html);}
 else if(path==='/test.js'){response.setHeader('Content-Type','text/javascript');response.end(source);}
 else if(/^\/[a-z-]+\.(js|css|html)$/.test(path)){
  response.setHeader('Content-Type',path.endsWith('.js')?'text/javascript':path.endsWith('.css')?'text/css':'text/html');
  response.end(readFileSync(join('platforms/web/site',path.slice(1))));
 }else{response.statusCode=404;response.end();}
});
function bitmap(){const b=Buffer.alloc(62);b.write('BM');b.writeUInt32LE(62,2);b.writeUInt32LE(54,10);b.writeUInt32LE(40,14);b.writeInt32LE(2,18);b.writeInt32LE(1,22);b.writeUInt16LE(1,26);b.writeUInt16LE(24,28);b.set([0,0,255,0,255,0],54);return b;}
let browser;
try{
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));browser=await chromium.launch();
 const context=await browser.newContext({acceptDownloads:true});const page=await context.newPage();
 await page.addInitScript(()=>{window.print=function(){parent.__printEvents??=[];parent.__printEvents.push({width:document.images[0]?.naturalWidth,height:document.images[0]?.naturalHeight,orientation:document.documentElement.dataset.printOrientation,page:getComputedStyle(document.body).page});};});
 await page.goto(`http://127.0.0.1:${server.address().port}/`);await page.waitForFunction(()=>window.ready);
 async function choose(request,file){await page.evaluate(value=>window.test.set(value),request);const waiting=page.waitForEvent('filechooser');await page.getByRole('button',{name:'Choose file…'}).click();await (await waiting).setFiles(file);}
 const image=bitmap();
 await choose({id:1,kind:1,title:'Import image',filter:'*.BMP'},{name:'synthetic.bmp',mimeType:'image/bmp',buffer:image});
 await page.waitForFunction(()=>test.replies.length===1);
 let result=await page.evaluate(()=>test.replies[0]);assert.deepEqual(result.raw,[...image]);assert.deepEqual(result.rgba,[255,0,0,255,0,255,0,255]);assert.equal(result.width,2);assert.equal(result.height,1);
 const png=Buffer.from(await page.evaluate(async()=>{const c=document.createElement('canvas');c.width=2;c.height=1;
 const ctx=c.getContext('2d');ctx.putImageData(new ImageData(Uint8ClampedArray.of(255,0,0,128,0,255,0,64),2,1),0,0);
 const blob=await new Promise(resolve=>c.toBlob(resolve,'image/png'));return [...new Uint8Array(await blob.arrayBuffer())];}));
 await choose({id:7,kind:1,title:'Import translucent image',filter:'*.PNG'},{name:'translucent.png',mimeType:'image/png',buffer:png});
 await page.waitForFunction(()=>test.replies.length===2);result=await page.evaluate(()=>test.replies[1]);assert.deepEqual(result.raw,[...png]);assert.deepEqual(result.rgba,[255,0,0,128,0,255,0,64]);
 const utf8=Buffer.from('\ufeff["grün €"]','utf8');
 await choose({id:2,kind:1,title:'Open car',filter:'*.CAR'},{name:'grün.CAR',mimeType:'text/plain',buffer:utf8});
 await page.waitForFunction(()=>test.replies.length===3);result=await page.evaluate(()=>test.replies[2]);assert.equal(result.name,'grün.CAR');assert.deepEqual(result.raw,[...new TextEncoder().encode('["grün €"]')]);
 const legacy=Buffer.from([91,34,103,114,252,110,32,128,32,223,34,93]);
 await choose({id:8,kind:1,title:'Open legacy car',filter:'*.CAR'},{name:'Ä 🚗.CAR',mimeType:'text/plain',buffer:legacy});
 await page.waitForFunction(()=>test.replies.length===4);result=await page.evaluate(()=>test.replies[3]);
 assert.equal(result.name,'Ä 🚗.CAR');assert.deepEqual(result.raw,[...new TextEncoder().encode('["grün € ß"]')]);
 const huge=Buffer.alloc(24);huge.set([137,80,78,71],0);huge.write('IHDR',12);huge.writeUInt32BE(5000,16);huge.writeUInt32BE(1,20);
 await choose({id:3,kind:1,title:'Import image',filter:'*.PNG'},{name:'too-wide.png',mimeType:'image/png',buffer:huge});
 await page.getByRole('alert').filter({hasText:'4096'}).waitFor();assert.equal(await page.evaluate(()=>test.files.open),true);await page.getByRole('button',{name:'Cancel'}).click();
 await page.evaluate(()=>test.set({id:4,kind:2,title:'Save car',name:'C:\\Cars\\old.CAR'}));
 await page.getByRole('textbox').fill('../escape.CAR');await page.getByRole('button',{name:'Save file',exact:true}).click();await page.getByRole('alert').filter({hasText:'short file name'}).waitFor();
 await page.getByRole('textbox').fill('Änne.CAR');await page.getByRole('button',{name:'Save file',exact:true}).click();
 await page.waitForFunction(()=>test.replies.length===5);
 const payload=Buffer.from('["grün € ß"]','utf8');await page.evaluate(data=>test.set({id:4,kind:3,title:'Save car',name:'Änne.CAR',data}),[...payload]);
 const waiting=page.waitForEvent('download');await page.getByRole('button',{name:'Download file'}).click();const download=await waiting;
 assert.equal(download.suggestedFilename(),'Änne.CAR');await download.saveAs(join(output,'synthetic.CAR'));assert.deepEqual(readFileSync(join(output,'synthetic.CAR')),Buffer.from([91,34,103,114,252,110,32,128,32,223,34,93]));
 await page.evaluate(data=>test.set({id:5,kind:4,title:'Synthetic print',name:'page.bmp',data}),[...image]);
 await page.getByRole('button',{name:'Print',exact:true}).waitFor();await page.waitForFunction(()=>!test.files.current.action.disabled);
 await page.getByRole('button',{name:'Print',exact:true}).click();await page.waitForFunction(()=>window.__printEvents?.length===1);
 assert.equal(await page.evaluate(()=>test.files.open),true);assert.deepEqual(await page.evaluate(()=>window.__printEvents[0]),{width:2,height:1,orientation:'landscape',page:'landscape'});
 const printFrame=page.frames().find(frame=>frame.url().endsWith('/print-image.html'));assert(printFrame);
 async function printedPage(frame,orientation){
  const value=await frame.evaluate(()=>({src:document.images[0].src,orientation:document.documentElement.dataset.printOrientation,
   margin:Array.from(document.styleSheets[0].cssRules).find(rule=>rule.selectorText===document.documentElement.dataset.printOrientation)?.style.margin}));
  assert.equal(value.orientation,orientation);assert.equal(value.margin,'0px');
  const preview=await page.context().newPage();await preview.goto(frame.url());
  await preview.evaluate(async({src,orientation})=>{document.documentElement.dataset.printOrientation=orientation;document.images[0].src=src;await document.images[0].decode();},value);
  const pdf=await preview.pdf({path:join(output,`${orientation}.pdf`),preferCSSPageSize:true,printBackground:true});await preview.close();
  // Chromium writes its page dictionaries outside compressed content streams.
  const dictionary=pdf.toString('latin1'),box=/\/MediaBox\s*\[0 0 ([\d.]+) ([\d.]+)\]/.exec(dictionary);
  assert(box,'printed PDF has a page MediaBox');assert.equal((dictionary.match(/\/Type \/Page\b/g)??[]).length,1);
  const w=Number(box[1]),h=Number(box[2]);assert.equal(w>h,orientation==='landscape');
  assert(Math.abs(Math.max(w,h)-842)<2&&Math.abs(Math.min(w,h)-595)<2,'printed page is A4');
 }
 await printedPage(printFrame,'landscape');
 await page.evaluate(()=>test.files.current.frame.contentWindow.dispatchEvent(new Event('afterprint')));
 await page.waitForFunction(()=>!test.files.open);
 const portrait=Buffer.from(image);portrait.writeInt32LE(1,18);portrait.writeInt32LE(2,22);
 await page.evaluate(data=>test.set({id:9,kind:4,title:'Portrait print',name:'portrait.bmp',data}),[...portrait]);
 await page.waitForFunction(()=>!test.files.current.action.disabled);await page.getByRole('button',{name:'Print',exact:true}).click();
 await page.waitForFunction(()=>window.__printEvents?.length===2);
 assert.deepEqual(await page.evaluate(()=>window.__printEvents[1]),{width:1,height:2,orientation:'portrait',page:'portrait'});
 await printedPage(page.frames().find(frame=>frame.url().endsWith('/print-image.html')),'portrait');
 await page.evaluate(()=>test.files.current.frame.contentWindow.dispatchEvent(new Event('afterprint')));await page.waitForFunction(()=>!test.files.open);
 await page.evaluate(()=>{test.set({id:6,kind:1,title:'Delayed read'});test.files.openFile(test.files.current,{name:'later.car',size:4,arrayBuffer:()=>new Promise(resolve=>window.finishRead=resolve)});});
 await page.getByRole('button',{name:'Cancel'}).click();await page.evaluate(()=>finishRead(Uint8Array.of(1,2,3,4).buffer));
 await page.waitForFunction(()=>!test.files.open);assert.equal(await page.evaluate(()=>test.replies.length),5);
 const state=await page.evaluate(()=>({holds:test.holds,completed:test.completed,cancelled:test.cancelled,replies:test.replies.length,allocationsFreed:test.freed.length}));
 assert.deepEqual(state.completed,[[4,1],[5,1],[9,1]]);assert.deepEqual(state.cancelled,[3,6]);assert.equal(state.holds.filter(([,on])=>on).length,state.holds.filter(([,on])=>!on).length);assert(state.allocationsFreed>0);
 await page.evaluate(()=>test.dispose());writeFileSync(join(output,'result.json'),JSON.stringify({status:'passing',browser:browser.version(),...state},null,2)+'\n');
 process.stdout.write('browser selected-file import/alpha, CAR encoding, save/download, bounded decode, cancellation and print handoff passed\n');
}finally{await browser?.close();await new Promise(resolve=>server.close(resolve));}
