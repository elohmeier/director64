import assert from 'node:assert/strict';
import test from 'node:test';
import {fileName,windows1252,carBytes,carExportBytes,imageDimensions,MAX_RGBA} from '../../platforms/web/site/user-files.js';

test('selected names use bounded UTF8 without paths, controls or replacement',()=>{
 assert.equal(fileName('Ärger € 🚗.CAR'),'Ärger € 🚗.CAR');
 assert.deepEqual([...windows1252('Ä€ß')],[196,128,223]);
 for(const name of ['', '.', '..', '../car', 'C:\\Car.car', 'folder/car', 'a:b', 'a\0b', 'a'.repeat(128),'ä'.repeat(64),'bad\ud800.car','a\u0081b'])
  assert.throws(()=>fileName(name));
});

test('CAR documents normalize legacy or modern input to UTF8 and export strict Windows1252',()=>{
 const encoded=new TextEncoder().encode('["grün € ß"]');
 const utf8=Uint8Array.of(239,187,191,...encoded),legacy=Uint8Array.of(91,34,103,114,252,110,32,128,32,223,34,93);
 assert.deepEqual(carBytes('car.CAR',utf8),encoded);assert.deepEqual(carBytes('car.car',legacy),encoded);
 assert.deepEqual(carExportBytes('car.car',encoded),legacy);
 const unsupported=new TextEncoder().encode('["🚗"]');assert.deepEqual(carBytes('car.car',unsupported),unsupported);
 assert.throws(()=>carExportBytes('car.car',unsupported),/unsupported/);
 assert.equal(carBytes('picture.png',utf8),utf8);assert.equal(carExportBytes('picture.png',utf8),utf8);
 const bmp=new Uint8Array(54),view=new DataView(bmp.buffer);bmp.set([66,77]);view.setUint32(14,40,true);view.setInt32(18,2,true);view.setInt32(22,1,true);
 assert.equal(carBytes('image.CAR',bmp),bmp);assert.equal(carExportBytes('image.CAR',bmp),bmp);
});

test('raster dimensions are checked from headers before a host decoder allocates pixels',()=>{
 const bmp=new Uint8Array(54),view=new DataView(bmp.buffer);bmp.set([66,77]);view.setUint32(14,40,true);
 view.setInt32(18,2,true);view.setInt32(22,-3,true);
 assert.deepEqual(imageDimensions(bmp),{width:2,height:3,mime:'image/bmp'});
 view.setInt32(18,-2,true);assert.throws(()=>imageDimensions(bmp),/4096/);
 const png=new Uint8Array(24),p=new DataView(png.buffer);png.set([137,80,78,71],0);png.set([73,72,68,82],12);
 p.setUint32(16,4096);p.setUint32(20,4096);assert.throws(()=>imageDimensions(png),/4096/);
 p.setUint32(16,2);p.setUint32(20,3);assert.equal(imageDimensions(png).width*imageDimensions(png).height*4,24);
 assert(MAX_RGBA<4096*4096*4);
 assert.equal(imageDimensions(new TextEncoder().encode('[1,2,3]')),null);
 assert.throws(()=>imageDimensions(Uint8Array.of(255,216,255,0)),/could not be opened/);
});
