import assert from "node:assert/strict";
import test from "node:test";
import {createHash} from "node:crypto";
import {Vfs} from "../../platforms/web/import/vfs.mjs";
import {sourcePolicy, installSystemFonts} from "../../platforms/web/import/pipeline.mjs";

test("original font installers enter the shared conversion namespace only after hash verification", () => {
  const raw = new TextEncoder().encode("MZ synthetic installer, never executed");
  const sha256 = (bytes) => createHash("sha256").update(bytes).digest("hex");
  const hash = sha256(raw), vfs = new Vfs();
  const path = "director64-system-fonts/example32.exe";
  const policy = {system_font_resources: [{path, sha256: hash}]};
  installSystemFonts(vfs, "/work/extracted/DATA", policy, {[hash]: raw}, sha256);
  assert.deepEqual(vfs.read(`/work/extracted/DATA/${path}`), raw);
  installSystemFonts(vfs, "/work/extracted/DATA", policy, {}, sha256);
  assert.throws(() => installSystemFonts(new Vfs(), "/media", policy, {}, sha256), /Missing original/);
  assert.throws(() => installSystemFonts(new Vfs(), "/media", policy, {[hash]: raw.subarray(1)}, sha256), /hash mismatch/);
  for (const unsafe of ["../font.exe", "/font.exe", "C:/font.exe", "a\\font.exe", ""])
    assert.throws(() => installSystemFonts(vfs, "/media", {system_font_resources: [{path: unsafe}]}, {}, sha256), /media-relative/);
});

test("disc aliases enter every conversion stage relative to the selected media root", () => {
  const policy = {director_version: 850, source_path_aliases: {stale: "old"}};
  const aliases = {
    "Long Root/Data/Levelwaage.dir": "LONGROOT/DATA/LEVELWAA.DIR",
    "Long Root/Data/Tamagotchi/00_a_Begruessung.dcr": "LONGROOT/DATA/TAMAGOTC/00ABEGRU.DCR",
    "Other/Outside.dir": "OTHER/OUTSIDE.DIR",
  };
  assert.deepEqual(sourcePolicy({port: {media_root: "/LONGROOT/DATA/"}, policy}, aliases), {
    director_version: 850,
    source_path_aliases: {"Levelwaage.dir": "LEVELWAA.DIR", "Tamagotchi/00_a_Begruessung.dcr": "TAMAGOTC/00ABEGRU.DCR"},
  });
  assert.deepEqual(sourcePolicy({port: {media_root: ""}, policy}, aliases).source_path_aliases, aliases);
  assert.deepEqual(policy.source_path_aliases, {stale: "old"});
});

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
