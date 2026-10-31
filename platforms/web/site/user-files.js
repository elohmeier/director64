// Player-selected files stay scoped to the game's explicit document request.
// Raw images are preserved; only optional decoded RGBA crosses the image bridge.
import {t} from "./strings.js";
export const MAX_RAW = 16 * 1024 * 1024, MAX_RGBA = 32 * 1024 * 1024;
const EXTRA = "€\u0081‚ƒ„…†‡ˆ‰Š‹Œ\u008dŽ\u008f\u0090‘’“”•–—˜™š›œ\u009džŸ";
export function windows1252(text) {
  const bytes=[];
  for(const character of text) {
    const cp=character.codePointAt(0), at=EXTRA.indexOf(character);
    if(cp<128 || cp>=160 && cp<=255)bytes.push(cp);
    else if(at>=0)bytes.push(at+128);
    else throw new Error(t("files.carEncoding"));
  }
  return Uint8Array.from(bytes);
}
export function fileName(value) {
  if(typeof value!=="string" || !value || value==='.' || value==='..' || /[\x00-\x1f\x7f-\x9f\\/:]/.test(value))
    throw new Error(t("files.nameInvalid"));
  for(const c of value)if(c.codePointAt(0)>=0xd800&&c.codePointAt(0)<=0xdfff)throw new Error(t("files.nameInvalid"));
  if(new TextEncoder().encode(value).length>=128)throw new Error(t("files.nameInvalid"));
  return value;
}
export function carBytes(name,raw) {
  if(!/\.car$/i.test(name)||imageDimensions(raw))return raw;
  let text;
  try{text=new TextDecoder("utf-8",{fatal:true}).decode(raw);}
  catch{text=new TextDecoder("windows-1252").decode(raw);}
  return new TextEncoder().encode(text);
}
export function carExportBytes(name,raw) {
  if(!/\.car$/i.test(name)||imageDimensions(raw))return raw;
  try{return windows1252(new TextDecoder("utf-8",{fatal:true}).decode(raw));}
  catch{throw new Error(t("files.carEncoding"));}
}
export function imageDimensions(raw) {
  const b=raw,v=new DataView(b.buffer,b.byteOffset,b.byteLength);
  const ascii=(at,size)=>String.fromCharCode(...b.subarray(at,at+size));
  const size=(width,height,mime)=>{
    if(width<1 || height<1 || width>4096 || height>4096 || width*height*4>MAX_RGBA)
      throw new Error(t("files.imageLarge"));
    return {width,height,mime};
  };
  if(b.length>=24 && b[0]===137 && ascii(1,3)==='PNG' && ascii(12,4)==='IHDR')
    return size(v.getUint32(16),v.getUint32(20),'image/png');
  if(b.length>=26 && ascii(0,2)==='BM') {
    const core=v.getUint32(14,true)===12;
    return size(core?v.getUint16(18,true):v.getInt32(18,true),
      Math.abs(core?v.getUint16(20,true):v.getInt32(22,true)),'image/bmp');
  }
  if(b.length>=10 && /^GIF8[79]a$/.test(ascii(0,6)))
    return size(v.getUint16(6,true),v.getUint16(8,true),'image/gif');
  if(b.length>=30 && ascii(0,4)==='RIFF' && ascii(8,4)==='WEBP') {
    const kind=ascii(12,4);
    if(kind==='VP8X')return size(1+b[24]+b[25]*256+b[26]*65536,1+b[27]+b[28]*256+b[29]*65536,'image/webp');
    if(kind==='VP8 ')return size(v.getUint16(26,true)&16383,v.getUint16(28,true)&16383,'image/webp');
    if(kind==='VP8L' && b[20]===47){const bits=v.getUint32(21,true);return size((bits&16383)+1,(bits>>>14&16383)+1,'image/webp');}
    throw new Error(t("files.imageInvalid"));
  }
  if(b.length>=3 && b[0]===255 && b[1]===216) {
    let at=2;
    while(at+4<=b.length) {
      if(b[at++]!==255)throw new Error(t("files.imageInvalid"));
      while(b[at]===255)at++;
      const marker=b[at++];
      if(marker===217 || marker===218)break;
      if(marker===1 || marker>=208 && marker<=215)continue;
      if(at+2>b.length)break;
      const length=v.getUint16(at);
      if(length<2 || at+length>b.length)break;
      if(marker>=192 && marker<=207 && ![196,200,204].includes(marker) && length>=7)
        return size(v.getUint16(at+5),v.getUint16(at+3),'image/jpeg');
      at+=length;
    }
    throw new Error(t("files.imageInvalid"));
  }
  return null;
}
async function pixels(raw,info) {
  const bitmap=await createImageBitmap(new Blob([raw],{type:info.mime}),
    {imageOrientation:'none',premultiplyAlpha:'none',colorSpaceConversion:'none'});
  try {
    if(bitmap.width!==info.width || bitmap.height!==info.height)throw new Error(t("files.imageInvalid"));
    const canvas=document.createElement('canvas');canvas.width=info.width;canvas.height=info.height;
    const context=canvas.getContext('2d',{willReadFrequently:true});
    context.drawImage(bitmap,0,0);
    return context.getImageData(0,0,info.width,info.height).data;
  } finally{bitmap.close();}
}
export class UserFiles {
  constructor({module,hold,swallow=()=>{},root=document.body}) {
    this.module=module;this.hold=hold;this.swallow=swallow;this.root=root;
    this.current=null;this.destinations=new Map();this.urls=new Set();this.disposed=false;
  }
  get open(){return !!this.current;}
  get element(){return this.current?.dialog??null;}
  string(pointer) {
    if(!pointer)return '';
    let end=pointer;const heap=this.module.HEAPU8;
    while(end<heap.length && heap[end] && end-pointer<1024)end++;
    const bytes=heap.subarray(pointer,end);
    try{return new TextDecoder('utf-8',{fatal:true}).decode(bytes);}
    catch{return new TextDecoder('windows-1252').decode(bytes);}
  }
  poll() {
    if(this.disposed || !this.module._d64_user_request_id)return false;
    const id=this.module._d64_user_request_id(),kind=this.module._d64_user_request_kind();
    if(this.current)return true;
    if(!id || !kind)return false;
    const request={id,kind,title:this.string(this.module._d64_user_title()),
      name:this.string(this.module._d64_user_name()),filter:this.string(this.module._d64_user_filter())};
    this.current=request;this.hold('user-file',true);this.swallow();
    this.show(request);
    return true;
  }
  show(request) {
    const node=(tag,text,kind)=>{const n=document.createElement(tag);if(text)n.textContent=text;if(kind)n.className=kind;return n;};
    const dialog=request.dialog=node('dialog',null,'dialog user-files');
    const title=node('h2',request.title || t(request.kind===1?'files.open':request.kind===2?'files.save':request.kind===3?'files.export':'files.print'),'heading');
    title.id=`user-file-title-${request.id}`;dialog.setAttribute('aria-labelledby',title.id);
    const error=request.error=node('p',null,'text warn');error.setAttribute('role','alert');error.hidden=true;
    const controls=node('div',null,'buttons');
    const cancel=node('button',t('button.cancel'));cancel.type='button';cancel.onclick=()=>this.cancel();
    const action=request.action=node('button',t(request.kind===1?'files.choose':request.kind===2?'files.save':request.kind===3?'files.download':'files.print'),'primary');action.type='button';
    controls.append(cancel,action);dialog.append(title,error,controls);this.root.append(dialog);
    dialog.addEventListener('cancel',event=>{event.preventDefault();this.cancel();});
    dialog.addEventListener('close',()=>{if(this.current===request)this.cancel();});
    if(request.kind===1) {
      const input=request.input=node('input');input.type='file';input.hidden=true;
      input.accept=[...new Set(request.filter.match(/\.[a-z0-9]+/gi)??[])].join(',');
      input.onchange=()=>{const file=input.files?.[0];if(file)this.openFile(request,file);};
      input.addEventListener('cancel',()=>this.cancel());
      dialog.insertBefore(input,controls);action.onclick=()=>input.click();
    } else if(request.kind===2) {
      const label=node('label',null,'field'),caption=node('span',t('files.name'));
      const input=request.input=node('input');input.type='text';input.maxLength=127;input.autocomplete='off';
      input.value=request.name.split(/[\\/:]/).at(-1) || 'document';
      label.append(caption,input);dialog.insertBefore(label,controls);
      action.onclick=()=>{try{const name=fileName(input.value);if(this.reply(request,name,new Uint8Array())){this.destinations.set(request.id,name);if(this.destinations.size>256)this.destinations.delete(this.destinations.keys().next().value);this.finish(request);}else throw new Error(t('files.rejected'));}
        catch(error){this.error(request,error);}};
      input.onkeydown=event=>{if(event.key==='Enter'){event.preventDefault();action.click();}};
    } else if(request.kind===3 || request.kind===4) {
      try {
        const size=this.module._d64_user_size(),at=this.module._d64_user_data();
        if(size>MAX_RAW || at<0 || at+size>this.module.HEAPU8.length)throw new Error(t('files.tooLarge'));
        request.data=this.module.HEAPU8.slice(at,at+size);
        request.name=this.destinations.get(request.id) || fileName(request.name || 'document.bmp');
        if(request.kind===3) {
          const description=node('p',request.name,'text');dialog.insertBefore(description,controls);
          action.onclick=()=>{try{this.download(request);this.complete(request);}catch(error){this.error(request,error);}};
        } else {
          const info=imageDimensions(request.data);if(!info)throw new Error(t('files.imageInvalid'));
          const image=request.preview=node('img');image.className='user-file-preview';image.alt=request.title || t('files.print');
          request.url=this.url(new Blob([request.data],{type:info.mime}));image.src=request.url;
          dialog.insertBefore(image,controls);action.disabled=true;
          image.decode().then(()=>{if(this.current===request)action.disabled=false;}).catch(error=>this.error(request,error));
          action.onclick=()=>this.print(request).catch(error=>this.error(request,error));
        }
      } catch(error){action.disabled=true;this.error(request,error);}
    } else {action.disabled=true;this.error(request,new Error(t('files.unsupported')));}
    dialog.showModal();
    if(request.kind===2){request.input.focus();request.input.select();}else action.focus();
  }
  error(request,error) {
    if(this.current!==request)return;
    request.error.textContent=String(error?.message??error);request.error.hidden=false;
    if(request.kind===1){request.action.disabled=false;request.input.value='';}
  }
  async openFile(request,file) {
    if(this.current!==request || request.action.disabled)return;
    request.action.disabled=true;request.error.hidden=true;
    try {
      const name=fileName(file.name);if(file.size>MAX_RAW)throw new Error(t('files.tooLarge'));
      const original=new Uint8Array(await file.arrayBuffer());
      if(this.current!==request)return;
      if(original.length>MAX_RAW)throw new Error(t('files.tooLarge'));
      const info=imageDimensions(original);
      if(!info && /\.(bmp|dib|png|jpe?g|gif|webp)/i.test(request.filter))throw new Error(t('files.imageInvalid'));
      const rgba=info?await pixels(original,info):null;
      if(this.current!==request)return;
      const raw=info?original:carBytes(name,original);if(raw.length>MAX_RAW)throw new Error(t('files.tooLarge'));
      if(this.reply(request,name,raw,rgba,info?.width??0,info?.height??0))this.finish(request);
      else throw new Error(t('files.rejected'));
    }catch(error){this.error(request,error);}
  }
  reply(request,name,raw,rgba=null,width=0,height=0) {
    const allocations=[],module=this.module;
    const copy=bytes=>{if(!bytes?.length)return 0;const at=module._malloc(bytes.length);if(!at)throw new Error(t('files.memory'));
      allocations.push(at);module.HEAPU8.set(bytes,at);return at;};
    try {
      const text=new TextEncoder().encode(name),terminated=new Uint8Array(text.length+1);terminated.set(text);
      return !!module._d64_user_reply(request.id,copy(terminated),copy(raw),raw.length,copy(rgba),width,height);
    } finally{for(const at of allocations)module._free(at);}
  }
  url(blob){const url=URL.createObjectURL(blob);this.urls.add(url);return url;}
  download(request) {
    const url=this.url(new Blob([carExportBytes(request.name,request.data)],{type:'application/octet-stream'}));
    const link=document.createElement('a');link.href=url;link.download=request.name;link.click();
    setTimeout(()=>{URL.revokeObjectURL(url);this.urls.delete(url);},10000);
  }
  async print(request) {
    if(this.current!==request)return;
    request.action.disabled=true;
    const frame=request.frame=document.createElement('iframe');frame.className='print-frame';frame.src='print-image.html';
    const loaded=new Promise((resolve,reject)=>{frame.onload=resolve;frame.onerror=()=>reject(new Error(t('files.printFailed')));});
    document.body.append(frame);await loaded;
    if(this.current!==request)return;
    const page=frame.contentDocument;page.title=request.title || t('files.print');
    const image=page.getElementById('picture');image.src=request.url;image.alt=request.title || t('files.print');await image.decode();
    if(this.current!==request)return;
    page.documentElement.dataset.printOrientation=image.naturalWidth>image.naturalHeight?'landscape':'portrait';
    await new Promise(resolve=>{frame.contentWindow.addEventListener('afterprint',resolve,{once:true});frame.contentWindow.print();});
    if(this.current===request)this.complete(request);
  }
  complete(request) {
    if(this.module._d64_user_complete(request.id,1))this.finish(request);
    else this.error(request,new Error(t('files.rejected')));
  }
  cancel(){if(this.current){const request=this.current;this.module._d64_user_cancel(request.id);this.finish(request);}}
  finish(request) {
    if(this.current!==request)return;
    this.current=null;request.frame?.remove();request.dialog.close();request.dialog.remove();
    if(request.url){URL.revokeObjectURL(request.url);this.urls.delete(request.url);}
    this.hold('user-file',false);this.swallow();
  }
  dispose(){this.cancel();this.disposed=true;for(const url of this.urls)URL.revokeObjectURL(url);this.urls.clear();}
}
