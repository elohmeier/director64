// Actual browser glyphs exercise the production text rasterizer with a CC0
// test font. No game fonts, converted assets or approved baselines are used.
import {chromium} from "playwright";
import {createServer} from "node:http";
import {readFileSync,writeFileSync,mkdirSync} from "node:fs";
import {join,resolve} from "node:path";
const output=resolve(process.argv[2]??"build/validation/browser/text-selftest");
mkdirSync(output,{recursive:true});
const html=`<!doctype html><script type="module">
import {textRasterizer} from '/player.js';
const bytes=new Uint8Array(await (await fetch('/monogram.ttf')).arrayBuffer());
const other=new Uint8Array(await(await fetch('/droid-sans.ttf')).arrayBuffer());
const fontBytes=new Uint8Array(bytes.length+other.length);fontBytes.set(bytes);fontBytes.set(other,bytes.length);
const text=await textRasterizer({index:{1:[0,bytes.length],2:[bytes.length,other.length]},blob:fontBytes});
const style={font:1,size:24,ascent:22,lineHeight:28};
const width=48,height=140;
const long=text('ABCDEFGHIJKLMNO',width,height,style);
let lower=0;for(let y=28;y<height;y++)for(let x=0;x<width;x++)lower+=long[y*width+x];
if(lower<100)throw new Error('long-word glyphs were clipped instead of wrapping');
const boxes=text.layout('ABCDEFGHIJKLMNO',width,height,style);
if(Math.max(...Array.from(boxes).filter((v,i)=>i%4===1))<28)throw new Error('long-word hit boxes did not wrap');
const plain=text('TEXT',120,40,style);
const bold=text('TEXT',120,40,{...style,face:1});
const underline=text('TEXT',120,40,{...style,face:4});
let boldDifference=0,underlineDifference=0;
for(let i=0;i<plain.length;i++){boldDifference+=Math.abs(plain[i]-bold[i]);underlineDifference+=Math.abs(plain[i]-underline[i]);}
if(boldDifference<100||underlineDifference<100)throw new Error('live font styles did not change actual glyph pixels');
const formats=[{offset:0,font:1,size:14,face:0,align:1,ascent:12,lineHeight:16,color:0xff0000},
{offset:4,font:2,size:30,face:3,align:1,ascent:27,lineHeight:36,color:0x0000ff}];
const rich=text.richText('III WWW WWW',150,100,formats),richBoxes=text.richLayout('III WWW WWW',150,100,formats);
let red=0,blue=0;for(let i=0;i<rich.length;i+=4)if(rich[i+3]){if(rich[i]>200&&rich[i+2]<20)red++;if(rich[i+2]>200&&rich[i]<20)blue++;}
if(red<20||blue<100||richBoxes[3]!==36||richBoxes[8*4+1]<36)throw new Error('mixed source font/size/face/color or shared wrapping failed');
const mixed=document.createElement('canvas');mixed.width=150;mixed.height=100;mixed.getContext('2d').putImageData(new ImageData(new Uint8ClampedArray(rich),150,100),0,0);
const trackingBase={offset:0,font:2,size:18,face:0,align:0,ascent:0,lineHeight:32,color:0};
const trackingText='ABCD'+String.fromCharCode(13)+'EF';
const normalBoxes=text.richLayout(trackingText,100,80,[trackingBase]);
const trackedBoxes=text.richLayout(trackingText,100,80,[{...trackingBase,charSpacing:2}]);
if(trackedBoxes[4]!==normalBoxes[4]+2||trackedBoxes[8]!==normalBoxes[8]+4||trackedBoxes[5*4+1]!==32||trackedBoxes[3]!==32)throw new Error('tracking or fixed line spacing did not reach actual browser layout');
const defaults={offset:0,font:2,size:0,face:0,align:0,ascent:0,lineHeight:0,color:0};
const zero=text.richText('ABCD',100,40,[defaults]),twelve=text.richText('ABCD',100,40,[{...defaults,size:12}]);
if(zero.some((value,i)=>value!==twelve[i]))throw new Error('zero point size did not render its12px default');
const unwrappedStyle={...trackingBase,lineHeight:28,layoutFlags:1};
const unwrappedText='ABC DEF GHI';
const wide=text.richText(unwrappedText,300,70,[unwrappedStyle]);
const clipped=text.richText(unwrappedText,40,70,[unwrappedStyle]);
const unwrappedBoxes=text.richLayout(unwrappedText,40,70,[unwrappedStyle]);
for(let y=0;y<70;y++)for(let x=0;x<40;x++)for(let c=0;c<4;c++)
 if(clipped[(y*40+x)*4+c]!==wide[(y*300+x)*4+c])throw new Error('no-wrap glyphs were reflowed instead of clipped');
if(unwrappedBoxes[8*4]<40||unwrappedBoxes[8*4+1]!==0)throw new Error('no-wrap lost offscreen character geometry');
const justifiedStyle={...trackingBase,align:3,lineHeight:28};
const fullBoxes=text.richLayout('AA BB CC DD',75,90,[justifiedStyle]);
if(fullBoxes[2*4+2]<=normalBoxes[2]||fullBoxes[6*4]!==0||fullBoxes[6*4+1]!==28)
 throw new Error('full justification lost expanded space boxes or final-row alignment');
const classicStyle={...trackingBase,lineHeight:0,layoutFlags:2};
const classic=text.richLayout('AB'+String.fromCharCode(13)+'CD',100,90,[classicStyle]);
const fixedClassic=text.richLayout('AB'+String.fromCharCode(13)+'CD',100,90,[{...classicStyle,lineHeight:32}]);
if(classic[3]!==fixedClassic[3]||fixedClassic[3*4+1]!==32||classic[3*4+1]<=0)
 throw new Error('classic measured cell and fixed row pitch were conflated');
const negative=text.richLayout('ABCD',100,80,[{...trackingBase,charSpacing:-1000}]);
const normalCaret=normalBoxes[3*4]+normalBoxes[3*4+2],negativeCaret=negative[3*4]+negative[3*4+2];
if(Math.abs(negativeCaret-(normalCaret-4000))>1||negative[3*4+2]>=0)
 throw new Error('negative tracking lost the signed final caret advance');
const canvas=document.createElement('canvas');canvas.width=width;canvas.height=height;
const rgba=new Uint8ClampedArray(width*height*4);for(let i=0;i<long.length;i++){rgba[i*4]=rgba[i*4+1]=rgba[i*4+2]=255;rgba[i*4+3]=long[i];}
canvas.getContext('2d').putImageData(new ImageData(rgba,width,height),0,0);
window.result={lower,boldDifference,underlineDifference,red,blue,richPng:mixed.toDataURL('image/png').split(',')[1],png:canvas.toDataURL('image/png').split(',')[1]};
</script>`;
const server=createServer((request,response)=>{
  const path=new URL(request.url,'http://localhost').pathname;
  if(path==='/droid-sans.ttf'){response.setHeader('Content-Type','font/ttf');response.end(readFileSync('third_party/fonts/droid-sans.ttf'));}
  else if(path==='/monogram.ttf'){response.setHeader('Content-Type','font/ttf');response.end(readFileSync('third_party/fonts/monogram.ttf'));}
  else if(/^\/[a-z-]+\.js$/.test(path)){response.setHeader('Content-Type','text/javascript');response.end(readFileSync(join('platforms/web/site',path.slice(1))));}
  else{response.setHeader('Content-Type','text/html');response.end(html);}
});
let browser;
try{
 await new Promise(resolve=>server.listen(0,'127.0.0.1',resolve));
 browser=await chromium.launch();const page=await browser.newPage();
 let pageError;page.on('pageerror',error=>{pageError=error;});
 await page.goto(`http://127.0.0.1:${server.address().port}`);
 await page.waitForFunction(()=>!!window.result);
 if(pageError)throw pageError;
 const {png,richPng,...result}=await page.evaluate(()=>window.result);
 writeFileSync(join(output,"mixed-glyphs.png"),Buffer.from(richPng,"base64"));
 writeFileSync(join(output,'wrapped-glyphs.png'),Buffer.from(png,'base64'));
 writeFileSync(join(output,'result.json'),JSON.stringify({status:'passing',browser:browser.version(),...result},null,2)+'\n');
 process.stdout.write('actual browser long-word glyphs, hit rows, bold and underline passed\n');
}finally{await browser?.close();await new Promise(resolve=>server.close(resolve));}
