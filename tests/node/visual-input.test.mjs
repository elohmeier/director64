import assert from "node:assert/strict";
import test from "node:test";
import {inputCommand, stateReply} from "../../platforms/web/site/validation.js";
import {expectation} from "../../tools/validation/browser.mjs";

test("recorded controller input has one logical sample and rejects player indices", () => {
  assert.equal(inputCommand("pad 60 80 0 1", 640, 480).single, "pad 1 80 0 1");
  for (let player = 0; player < 4; player++)
    assert.throws(() => inputCommand(`pad ${player} 60 80 0 1`, 640, 480));
});

test("browser validation accepts only bounded real player input", () => {
  assert.equal(inputCommand("step 60 799 599 1", 800, 600).single, "step 1 799 599 1");
  assert.equal(inputCommand("text 127 ANNA", 800, 600).command, "text 127 ANNA");
  assert.equal(inputCommand("key 36 13 1", 800, 600).command, "key 36 13 1");
  for (const command of ["movie SECRET.DXR", "hide 7", "inspect global", "step 1 800 0 0", "step 1 0 -1 0", "pad 1 128 0 0", "text 0 ANNA", "state\nmovie SECRET.DXR"])
    assert.throws(() => inputCommand(command, 800, 600));
});

test("browser oracle fails script alerts, compositor errors and unexpected exits", () => {
  assert.deepEqual(stateReply('ENTER START\n{"movie":"START","error":""}\n'), {movie:"START",error:""});
  for (const value of [{error:"missing font"}, {script_errors:1,last_script_error:"bad script"}, {quit:true}])
    assert.throws(() => stateReply(JSON.stringify(value)));
});

test("browser journey expectations cannot pass absent or wrong state", () => {
  expectation({movie:"ROOM",saves:{"profile/a.txt":"ANNA"}}, {movie:"ROOM","/saves/profile~1a.txt":{contains:"ANNA"}});
  for (const wanted of [{movie:"OTHER"},{missing:{gte:1}},{movie:{typo:true}}])
    assert.throws(() => expectation({movie:"ROOM"},wanted));
});

test("skipped conditional checkpoint cannot silently become an earlier observation", async () => {
  const {requireCheckpoints} = await import("../../tools/validation/browser.mjs");
  const plan = {steps: [{label:"target",when:{movie:"OTHER"}}],scope:{ends_at_checkpoint:"target"},observations:{target:{}}};
  assert.throws(()=>requireCheckpoints(plan,[{label:"earlier"}]),/target.*not exercised/);
  assert.deepEqual(requireCheckpoints(plan,[{label:"target"}]),["target"]);
  assert.throws(()=>requireCheckpoints({steps:[{observe:{label:"mask-window"}}]},[]),/mask-window/);
});

test("decoder finalization observes errors raised after the last successful input", async () => {
  const {finalizeBrowser} = await import("../../tools/validation/browser.mjs");
  const previous = globalThis.window;
  let late = false;
  globalThis.window = {__validationDecodePromises:new Set(),__validationDecoders:new Set([
    {state:"configured",flush:async()=>{await Promise.resolve();late=true;}}]),
    director64:{audio:{decoding:new Map()},validation:{state(){if(late)throw new Error("late media decode failure");return {};}}}};
  try {await assert.rejects(finalizeBrowser({evaluate:fn=>fn()}),/late media decode failure/);}
  finally {if(previous===undefined)delete globalThis.window;else globalThis.window=previous;}
});

test("browser observation uses the existing input tick budget and transfers artifacts afterwards", async () => {
  const {observationWindow} = await import("../../tools/validation/browser.mjs");
  const previous = globalThis.window;
  let tick=0;
  const seen=[];
  globalThis.window={director64:{validation:{
    async advance(command){if(command.startsWith("step"))tick+=Number(command.split(" ")[1]);},
    startAudio:async()=>{},takeAudio:()=>({pcm:"",rate:48000,channels:2}),state:()=>({tick}),
    capture:()=>{seen.push(tick);return {state:{tick}};},timing:()=>({max_lateness_ms:0}),
  }}};
  try {
    const window=await observationWindow({frames:4,stride:5,x:0,y:0,firstTick:1});
    assert.equal(window.ticks,16);
    assert.deepEqual(seen,[1,6,11,16]);
    assert.equal(window.pictures.length,4);
  }finally{if(previous===undefined)delete globalThis.window;else globalThis.window=previous;}
});

test("host timing failures retain image evidence and cannot rush an input backlog", async () => {
  const {observationWindow} = await import("../../tools/validation/browser.mjs");
  const previous=globalThis.window;
  let ticks=0,resets=0;
  globalThis.window={director64:{validation:{
    async advance(command){if(command==='state')resets++;else ticks+=Number(command.split(' ')[1]);},
    startAudio:async()=>{},takeAudio:()=>({pcm:'',rate:48000,channels:2}),state:()=>({ticks}),
    capture:()=>({state:{ticks}}),timing:()=>({max_lateness_ms:350}),
  }}};
  try{
    const result=await observationWindow({frames:4,stride:5,x:0,y:0,firstTick:1});
    assert.equal(result.ticks,16);assert.equal(ticks,16);
    assert.equal(result.pictures.length,4);
    assert.ok(result.timingFindings.length&&resets>1);
  }finally{if(previous===undefined)delete globalThis.window;else globalThis.window=previous;}
});
