import assert from "node:assert/strict";
import test from "node:test";
import {Vfs} from "../../platforms/web/import/vfs.mjs";
import {PackageCache} from "../../platforms/web/site/cache.js";

// What the Rust converter's host files (compiler/wasm HostFiles) ask of the
// import's file system: a disk's semantics under POSIX paths.
test("the in-memory file system behaves as the converter expects of a disk", () => {
  const vfs = new Vfs();
  assert.throws(() => vfs.write("/missing/a", Uint8Array.of(1)), {code: "ENOENT"});
  vfs.mkdir("/w/sub", true);
  vfs.write("/w/sub/b.txt", new TextEncoder().encode("bee"));
  vfs.write("/w/a.bin", Uint8Array.of(1, 2, 3));
  assert.deepEqual(vfs.list("/w"), [["a.bin", "file"], ["sub", "dir"]]);
  assert.equal(new TextDecoder().decode(vfs.read("/w/./sub/../sub/b.txt")), "bee");
  assert.ok(vfs.isFile("/w/a.bin") && !vfs.isFile("/w/sub") && vfs.exists("/w/sub"));
  assert.throws(() => vfs.read("/w/none"), {code: "ENOENT"});
  assert.throws(() => vfs.list("/w/none"), {code: "ENOENT"});
  vfs.remove("/w/sub");
  assert.deepEqual(vfs.list("/w"), [["a.bin", "file"]]);
  assert.equal(vfs.bytes(), 3);
  // A lazy file has its size at once and its bytes only when read.
  let loads = 0;
  vfs.writeLazy("/w/big.scr", 1000, () => { loads++; return new Uint8Array(1000); });
  assert.equal(vfs.size("/w/big.scr"), 1000);
  assert.ok(vfs.isFile("/w/big.scr") && loads === 0 && vfs.bytes() === 3);
  assert.equal(vfs.read("/w/big.scr").length, 1000);
  assert.equal(loads, 1);
});

// A fake origin-private directory tree with failure injection.
function directory(state = {fail: null}) {
  const files = new Map(), dirs = new Map();
  const handle = {
    kind: "directory", files, dirs, state,
    async getDirectoryHandle(name, options = {}) {
      if (!dirs.has(name)) {
        if (!options.create) throw Object.assign(new Error("missing"), {name: "NotFoundError"});
        dirs.set(name, directory(state));
      }
      return dirs.get(name);
    },
    async getFileHandle(name, options = {}) {
      if (!files.has(name) && !options.create) throw Object.assign(new Error("missing"), {name: "NotFoundError"});
      return {
        async getFile() {
          const data = files.get(name) ?? new Uint8Array();
          return {size: data.length, arrayBuffer: async () => data.slice().buffer};
        },
        async createWritable() {
          let staged;
          return {
            async write(bytes) {
              if (state.fail === name) throw Object.assign(new Error("quota"), {name: "QuotaExceededError"});
              staged = new Uint8Array(bytes);
            },
            async close() { files.set(name, staged); },
            async abort() {},
          };
        },
      };
    },
    async removeEntry(name) {
      if (!dirs.delete(name) && !files.delete(name)) throw Object.assign(new Error("missing"), {name: "NotFoundError"});
    },
    async *entries() {
      for (const [name, value] of dirs) yield [name, value];
      for (const name of files.keys()) yield [name, {kind: "file"}];
    },
  };
  return handle;
}
function cache(key, root = directory()) {
  const c = new PackageCache("game", key);
  c.root = root;
  return c;
}
const game = () => ({
  package: Uint8Array.of(1, 2, 3),
  images: {blob: Uint8Array.of(4, 5), index: {"a.fdi": [0, 2]}},
  audio: {blob: Uint8Array.of(6), index: {"s.wav64": [0, 1]}},
  fonts: {blob: Uint8Array.of(7, 8), index: {"1.font64": [0, 2]}},
});

test("a converted game round-trips, committed by its manifest", async () => {
  const c = cache("k1");
  assert.equal(await c.load(), null);
  await c.store(game(), {stages: {}});
  const loaded = await c.load();
  assert.deepEqual([...loaded.package], [1, 2, 3]);
  assert.deepEqual(loaded.images.index, {"a.fdi": [0, 2]});
  assert.equal(loaded.manifest.key, "k1");
  // A truncated file no longer matches its manifest.
  c.root.dirs.get("k1").files.set("audio.bin", new Uint8Array());
  assert.equal(await c.load(), null);
});

test("entries of other converters and interrupted imports are pruned; nothing else", async () => {
  const root = directory();
  const current = cache("current", root);
  await current.store(game(), {});
  await cache("older", root).store(game(), {});
  await root.getDirectoryHandle("interrupted", {create: true});
  await current.prune();
  assert.deepEqual([...root.dirs.keys()], ["current"]);
  assert.ok(await current.load());
});

test("a failed write leaves no entry and reports the error", async () => {
  const root = directory({fail: "images.bin"});
  const c = cache("k", root);
  await assert.rejects(c.store(game(), {}), {name: "QuotaExceededError"});
  assert.deepEqual([...root.dirs.keys()], []);
  assert.equal(await c.load(), null);
});
