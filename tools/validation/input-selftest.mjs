// Source-free Chromium component proof of the production controller selector
// and native/software cursor policy. Device samples are synthetic; this does
// not qualify a physical gamepad, Android PointerIcon or original cursor pixels.
import assert from "node:assert/strict";
import {chromium} from "playwright";
import {createServer} from "node:http";
import {readFileSync, writeFileSync, mkdirSync} from "node:fs";
import {join, resolve} from "node:path";
const output = resolve(process.argv[2] ?? "build/validation/browser/input-selftest");
mkdirSync(output, {recursive: true});
const html = `<!doctype html><button id="press">Press primary</button><button id="release">Release primary</button>
<button id="menu">Open menu</button><button id="extra">Connect extra controller</button>
<button id="disconnect">Disconnect primary</button><canvas width="100" height="100"></canvas><script type="module">
import {Gamepads, BUTTON} from '/gamepad.js';
import {cursorPresentation} from '/cursor.js';
const pad=(index,buttons=[],axes=[0,0,0,0])=>({index,id:'Synthetic '+index,connected:true,mapping:'standard',axes,
 buttons:Array.from({length:18},(_,n)=>({pressed:buttons.includes(n),value:buttons.includes(n)?1:0}))});
let devices=[pad(1),pad(3,[BUTTON.cross],[1,0,0,0])],time=0;
const input=new Gamepads(()=>devices),events=[],canvas=document.querySelector('canvas');
input.addEventListener('connected',()=>events.push('connected'));
input.addEventListener('disconnected',()=>events.push('disconnected'));
const poll=()=>{input.poll(++time);return {device:input.device?.index,sample:input.sample,actions:[...input.actions],connected:input.connected};};
document.querySelector('#press').onclick=()=>{devices=devices.map(p=>p.index===1?pad(1,[BUTTON.cross]):p);poll();};
document.querySelector('#release').onclick=()=>{devices=devices.map(p=>p.index===1?pad(1):p);poll();};
document.querySelector('#menu').onclick=()=>{input.swallow();poll();};
document.querySelector('#extra').onclick=()=>{devices.unshift(pad(0,[BUTTON.down]));poll();};
document.querySelector('#disconnect').onclick=()=>{devices=devices.filter(p=>p.index!==1);poll();};
canvas.onpointermove=event=>window.pointerType=event.pointerType;
window.test={read:()=>({device:input.device?.index,sample:input.sample,actions:[...input.actions],events:[...events]}),
 role(role,mouse){const value=cursorPresentation(role,mouse);canvas.style.cursor=value.css;return {...value,computed:getComputedStyle(canvas).cursor};}};
poll();window.ready=true;
</script>`;
const server = createServer((request, response) => {
  if (request.url === "/gamepad.js" || request.url === "/cursor.js") {
    response.setHeader("Content-Type", "text/javascript");
    response.end(readFileSync(join("platforms/web/site", request.url.slice(1))));
  } else { response.setHeader("Content-Type", "text/html"); response.end(html); }
});
let browser;
try {
  await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
  browser = await chromium.launch();
  const page = await browser.newPage();
  const errors = []; page.on("pageerror", error => errors.push(String(error)));
  await page.goto(`http://127.0.0.1:${server.address().port}/`);
  await page.waitForFunction(() => window.ready);
  assert.deepEqual(await page.evaluate(() => test.read()), {device: 1,
    sample: {buttons: 0, stickX: 0, stickY: 0}, actions: [], events: ["connected"]});
  await page.getByRole("button", {name: "Connect extra controller"}).click();
  assert.equal((await page.evaluate(() => test.read())).device, 1);
  assert.deepEqual((await page.evaluate(() => test.read())).actions, []);
  await page.getByRole("button", {name: "Press primary", exact: true}).click();
  assert.deepEqual((await page.evaluate(() => test.read())).actions, ["accept"]);
  await page.getByRole("button", {name: "Open menu"}).click();
  const swallowed = await page.evaluate(() => test.read());
  assert.equal(swallowed.sample.buttons, 0); assert.deepEqual(swallowed.actions, []);
  await page.getByRole("button", {name: "Release primary"}).click();
  await page.getByRole("button", {name: "Press primary", exact: true}).click();
  assert.deepEqual((await page.evaluate(() => test.read())).actions, ["accept"]);
  await page.getByRole("button", {name: "Disconnect primary"}).click();
  const replaced = await page.evaluate(() => test.read());
  assert.equal(replaced.device, 0); assert.deepEqual(replaced.actions, ["down"]);
  assert.deepEqual(replaced.events, ["connected", "disconnected", "connected"]);
  const cursors = [];
  for (const [role, css] of [[0,"default"],[1,"text"],[2,"crosshair"],[3,"crosshair"],[4,"wait"],[-1,"none"],[200,"none"]]) {
    const result = await page.evaluate(role => test.role(role, true), role);
    assert.equal(result.css, css); assert.equal(result.computed, css);
    assert.equal(result.software, role === -1); cursors.push({role, ...result});
  }
  const bounds = await page.locator("canvas").boundingBox();
  await page.mouse.move(bounds.x + 20, bounds.y + 20);
  assert.equal(await page.evaluate(() => window.pointerType), "mouse");
  assert.deepEqual(await page.evaluate(() => test.role(0, false)), {css:"none",software:true,computed:"none"});
  assert.deepEqual(errors, []);
  const report = {status:"passing",scope:"source-free browser components; synthetic gamepads",
    browser:browser.version(),checks:10,cursors,errors};
  writeFileSync(join(output,"report.json"),JSON.stringify(report,null,2)+"\n");
  console.log("browser input: one selected controller, menu swallowing, disconnect and OS/software cursor roles passed");
} finally { await browser?.close(); await new Promise(resolve => server.close(resolve)); }
