import assert from "node:assert/strict";
import {mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync} from "node:fs";
import {tmpdir} from "node:os";
import {join} from "node:path";
import {spawnSync} from "node:child_process";
import test from "node:test";

// A synthetic Wasm embedder: the RPC succeeds and only rendering reveals the
// unsupported ink. This is the precise false-success path the gate closes.
function probe(commands, options = [], bootError = false, {measure = false, strict = false} = {}) {
  const site = mkdtempSync(join(tmpdir(), "director64-probe-"));
  try {
    writeFileSync(join(site, "build.json"), JSON.stringify({game: {entry_movie: "START"}}));
    writeFileSync(join(site, "director64.mjs"), `
      export default async ({director64Host: host}) => {
        let failing = ${bootError}, error = "", renderTicks = false, alerts = 0;
        const api = {
          HEAPU8: new Uint8Array(16), _d64_flash: () => 0, _d64_flash_bytes: () => 16,
          _d64_probe_render: enabled => { renderTicks = !!enabled; },
          UTF8ToString: value => value, _d64_error: () => error,
          _d64_script_errors: () => alerts, _d64_last_script_error: () => "recovered script error",
          _d64_render: () => {
            process.stderr.write("render\\n");
            if (!host.text("text", 10, 10, null)) throw new Error("missing text rasterizer");
            if (failing) error = "unsupported native bitmap ink";
            return 0; // An unchanged frame legitimately has no new pixel buffer.
          },
          ccall: (name, result, types, args) => {
            if (args[0] === "bad") failing = true;
            if (args[0] === "alert") alerts++;
            if (args[0] === "step 3 0 0 0") {
              for (let i = 0; i < 3 && !error; i++) {
                failing = i === 1; // Transient bad sprite is gone by the last tick.
                if (renderTicks) api._d64_render();
              }
              failing = false;
            }
            return name === "d64_boot_rpc" ? "BOOT\\n" : "OK\\n";
          },
        };
        return api;
      };
    `);
    const metrics = join(site, "metrics");
    const result = spawnSync(process.execPath,
      ["platforms/web/node/probe.mjs", site, "START", "rpc", ...options],
      {input: commands, encoding: "utf8", env: {...process.env,
        DIRECTOR64_PROBE_METRICS: measure ? metrics : "",
        DIRECTOR64_PROBE_STRICT: strict ? "1" : ""}});
    if (measure) {
      result.metrics = readdirSync(metrics).filter(name => name.endsWith(".jsonl")).flatMap(name =>
        readFileSync(join(metrics, name), "utf8").trim().split("\n").map(JSON.parse));
      result.commands = readdirSync(metrics).filter(name => name.endsWith(".commands.txt"))
        .map(name => readFileSync(join(metrics, name), "utf8"));
    }
    return result;
  } finally {
    rmSync(site, {recursive: true, force: true});
  }
}

test("RPC probe renders boot and each command, including unchanged frames", () => {
  const result = probe("first\nsecond\n");
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, "BOOT\nOK\nOK\n");
  assert.equal(result.stderr, "render\nrender\nrender\n");
});

test("successful RPC state cannot hide a compositor error", () => {
  const result = probe("bad\n");
  assert.equal(result.status, 1);
  assert.equal(result.stdout, "BOOT\n");
  assert.match(result.stderr, /runtime failure after "bad": unsupported native bitmap ink/);
});

test("boot rendering failures fail before reporting successful startup", () => {
  const result = probe("", [], true);
  assert.equal(result.status, 1);
  assert.equal(result.stdout, "");
  assert.match(result.stderr, /runtime failure after boot/);
});

test("state-only diagnostic opt-out is explicit", () => {
  const result = probe("bad\n", ["--state-only"]);
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, "BOOT\nOK\n");
  assert.equal(result.stderr, "");
});

test("rendered RPC mode checks transient frames inside a multi-tick command", () => {
  const result = probe("step 3 0 0 0\n");
  assert.equal(result.status, 1);
  assert.match(result.stderr, /runtime failure after "step 3 0 0 0"/);
  const stateOnly = probe("step 3 0 0 0\n", ["--state-only"]);
  assert.equal(stateOnly.status, 0);
});

test("performance telemetry preserves RPC output and measures boot plus commands", () => {
  const result = probe("first\nsecond\n", [], false, {measure: true});
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, "BOOT\nOK\nOK\n");
  assert.deepEqual(result.metrics.map(sample => sample.command), ["boot", "first", "second"]);
  assert.deepEqual(result.commands, ["first\nsecond\n"]);
  for (const sample of result.metrics) {
    assert.ok(sample.elapsed_ms >= 0);
    assert.ok(sample.cpu_ms >= 0);
    assert.ok(sample.peak_rss_bytes > 0);
    assert.equal(sample.wasm_bytes, 16);
  }
});

test("failed renders retain a timing sample without a successful state reply", () => {
  const result = probe("bad\n", [], false, {measure: true});
  assert.equal(result.status, 1);
  assert.equal(result.stdout, "BOOT\n");
  assert.deepEqual(result.metrics.map(sample => sample.command), ["boot", "bad"]);
  assert.deepEqual(result.commands, ["bad\n"]);
});

test("journey strict mode treats recovered script alerts as failures", () => {
  const result = probe("alert\n", [], false, {measure: true, strict: true});
  assert.equal(result.status, 1);
  assert.equal(result.stdout, "BOOT\n");
  assert.match(result.stderr, /script alert after "alert": recovered script error/);
  assert.equal(result.metrics.at(-1).script_errors, 1);
  assert.equal(probe("alert\n").status, 0); // Fuzzing keeps its separate alert policy.
});
