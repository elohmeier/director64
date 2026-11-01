import assert from "node:assert/strict";
import {test} from "node:test";
import {mkdtempSync, readFileSync, readdirSync, rmSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {probeMetrics} from "../../platforms/web/node/metrics.mjs";

test("per-command renderer work resets before execution and is retained with host costs", () => {
  const folder=mkdtempSync(join(tmpdir(),"d64-work-"));
  try {
    let resets=0;
    const work={version:1,frames:9,peak_frame_pixels:480000,worst_inverse:{pixels:0}};
    const module={HEAPU8:new Uint8Array(16),UTF8ToString:value=>value,
      _d64_render_work_reset:()=>{resets++;},_d64_render_work_json:()=>JSON.stringify(work)};
    const metrics=probeMetrics(folder);
    const start=metrics.start("step 60 0 0 0",module);
    assert.equal(resets,1);
    metrics.end(start,"step 60 0 0 0",module);
    const file=readdirSync(folder).find(name=>name.endsWith(".jsonl"));
    const sample=JSON.parse(readFileSync(join(folder,file),"utf8"));
    assert.deepEqual(sample.render_work,work);
    assert.ok(sample.elapsed_ms>=0 && sample.cpu_ms>=0);
    assert.equal(sample.command,"step 60 0 0 0");
  } finally {rmSync(folder,{recursive:true,force:true});}
});

test("disabled metrics do not enable renderer profiling", () => {
  const metrics=probeMetrics("");
  metrics.start("step 1 0 0 0",{_d64_render_work_reset:()=>{throw new Error("unexpected reset");}});
  metrics.end();
});
