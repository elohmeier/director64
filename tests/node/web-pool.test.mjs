import assert from "node:assert/strict";
import test from "node:test";
import {answer, ProxyFiles} from "../../platforms/web/import/jobs.mjs";
import {Pool} from "../../platforms/web/import/pool.mjs";
import {Vfs} from "../../platforms/web/import/vfs.mjs";

// A movie job's files on a pool worker: its own outputs stay local, every
// other question is answered by the import worker or recorded as missing.
test("a pool worker's files keep outputs local and ask for everything else", () => {
  const vfs = new Vfs();
  vfs.mkdir("/work/analysis", true);
  vfs.write("/work/analysis/a.dump", Uint8Array.of(7, 8));
  const answers = new Map();
  const first = new ProxyFiles(["/work/director"], answers);
  assert.throws(() => first.read("/work/analysis/a.dump"), {code: "ENOENT"});
  assert.equal(first.exists("/work/analysis/b.dump"), false);
  first.write("/work/director/images/x.fdi", Uint8Array.of(1));
  assert.deepEqual(first.read("/work/director/images/x.fdi"), Uint8Array.of(1));
  assert.equal(first.isFile("/work/director/images/y.fdi"), false);
  assert.deepEqual([...first.missing.keys()], ["read /work/analysis/a.dump", "exists /work/analysis/b.dump"]);
  // The import worker answers, absent files included; the next run has them.
  for (const [key, value] of answer(vfs, [...first.missing.values()]).answers) answers.set(key, value);
  const second = new ProxyFiles(["/work/director"], answers);
  assert.deepEqual(second.read("/work/analysis/a.dump"), Uint8Array.of(7, 8));
  assert.equal(second.exists("/work/analysis/b.dump"), false);
  assert.equal(second.missing.size, 0);
  second.write("/work/director/model.json", Uint8Array.of(9));
  assert.deepEqual(second.written().map(([path]) => path), ["/work/director/model.json"]);
});

// In-process stand-ins for workers: "double" answers n * 2 after a delay that
// makes later jobs finish first; "ask" fetches a file through need/provide.
function fakeWorker() {
  let handler = null;
  const reply = (message) => setTimeout(() => handler(message), 0);
  return {
    receive: (h) => { handler = h; },
    send(message) {
      if (message.type === "init") return reply({type: "ready"});
      if (message.type === "provide") return reply({id: message.id, result: message.answers[0][1].value});
      const {id, kind, payload} = message;
      if (kind === "double") setTimeout(() => handler({id, result: payload * 2}), (10 - payload) * 3);
      else if (kind === "ask") reply({type: "need", id, requests: [{op: "size", path: payload}]});
      else reply({id, error: `unknown ${kind}`});
    },
    close() {},
  };
}

test("the pool hands every result to its input's index, in any finishing order", async () => {
  const pool = new Pool([fakeWorker(), fakeWorker(), fakeWorker()], {type: "init"});
  const results = new Array(10);
  const progress = [];
  await pool.map("double", 10, (i) => i, (i, value) => { results[i] = value; }, {onProgress: (f) => progress.push(f)});
  assert.deepEqual(results, [0, 2, 4, 6, 8, 10, 12, 14, 16, 18]);
  assert.equal(progress.at(-1), 1);
});

test("a job's questions about the import's files are answered by the map's provider", async () => {
  const vfs = new Vfs();
  vfs.mkdir("/d", true);
  vfs.write("/d/f", new Uint8Array(5));
  const pool = new Pool([fakeWorker()], {type: "init"});
  const sizes = [];
  await pool.map("ask", 1, () => "/d/f", (i, size) => { sizes[i] = size; },
    {provide: (requests) => answer(vfs, requests)});
  assert.deepEqual(sizes, [5]);
});

test("a failing job rejects the map", async () => {
  const pool = new Pool([fakeWorker(), fakeWorker()], {type: "init"});
  await assert.rejects(pool.map("nope", 3, (i) => i, () => {}), /unknown nope/);
});
