// Real-browser replay of input-only journey steps. Game data and screenshots
// remain under ignored build/. This module is also importable by unit tests.
import {createServer} from "node:http";
import {readFileSync, writeFileSync, mkdirSync, statSync} from "node:fs";
import {resolve, join, extname, sep} from "node:path";
import {pathToFileURL} from "node:url";

export function expectation(state, wanted) {
  for (const [path, expected] of Object.entries(wanted ?? {})) {
    const keys = path.startsWith("/") ? path.slice(1).split("/").map(s => s.replaceAll("~1", "/").replaceAll("~0", "~")) : path.split(".");
    const actual = keys.reduce((v, key) => v && typeof v === "object" ? v[key] : undefined, state);
    let ok;
    if (expected && typeof expected === "object" && !Array.isArray(expected)) {
      const ops = Object.keys(expected);
      if (ops.length !== 1 || !["gte", "contains", "in"].includes(ops[0])) throw new Error(`unknown expectation ${path}`);
      if (ops[0] === "gte") ok = typeof actual === "number" && actual >= expected.gte;
      if (ops[0] === "contains") ok = actual != null && actual.includes?.(expected.contains);
      if (ops[0] === "in") ok = expected.in.includes(actual);
    } else ok = JSON.stringify(actual) === JSON.stringify(expected);
    if (!ok) throw new Error(`${path}: expected ${JSON.stringify(expected)}, got ${JSON.stringify(actual)}`);
  }
}

export function requireCheckpoints(plan, checkpoints) {
  const reached = new Set(checkpoints.map(c => c.label));
  const required = new Set(Object.keys(plan.observations ?? {}));
  if (plan.scope?.ends_at_checkpoint) required.add(plan.scope.ends_at_checkpoint);
  for (const step of plan.steps) if (step.observe?.label) required.add(step.observe.label);
  for (const label of required)
    if (!reached.has(label)) throw new Error(`requested checkpoint ${label} was not exercised`);
  return [...required];
}

export function trackDecoders() {
  window.__validationDecoders = new Set();
  window.__validationDecodePromises = new Set();
  for (const name of ["AudioDecoder", "VideoDecoder"]) {
    if (!window[name]) continue;
    window[name] = new Proxy(window[name], {construct(target, args) {
      const decoder = Reflect.construct(target, args);
      window.__validationDecoders.add(decoder);
      return decoder;
    }});
  }
  const original = AudioContext.prototype.decodeAudioData;
  AudioContext.prototype.decodeAudioData = function(...args) {
    const result = original.apply(this, args);
    if (result?.then) {
      window.__validationDecodePromises.add(result);
      result.then(() => window.__validationDecodePromises.delete(result),
        () => window.__validationDecodePromises.delete(result));
    }
    return result;
  };
}

export async function finalizeBrowser(page) {
  return page.evaluate(async () => {
    let timer;
    try {
      await Promise.race([
        (async () => {
          await Promise.all([...window.__validationDecodePromises ?? [],
            ...window.director64.audio.decoding?.values() ?? []]);
          await Promise.all([...window.__validationDecoders ?? []]
            .filter(decoder => decoder.state === "configured").map(decoder => decoder.flush()));
          await new Promise(resolve => setTimeout(resolve, 0));
          // The validation state adapter checks both real host error queues.
          return window.director64.validation.state();
        })(),
        new Promise((_, reject) => { timer = setTimeout(() => reject(new Error("decoder finalization timed out")), 30000); }),
      ]);
      return window.director64.validation.state();
    } finally { clearTimeout(timer); }
  });
}

export async function observationWindow({frames, stride, x, y, buttons = 0, firstTick = 0,
  audioTicks = 0, maxLateness = 200}) {
  const api = window.director64.validation;
  // Artifact transport belongs outside the timed window. Reset only the test
  // clock epoch, through a read-only state request; authored state is untouched.
  await api.advance("state", false);
  await api.startAudio();
  let ticks = 0;
  const timingFindings = [];
  const send = async count => {
    for (let left = count; left > 0; left -= 4) {
      const n = Math.min(4, left);
      await api.advance(`step ${n} ${x} ${y} ${buttons}`, true);
      ticks += n;
      if (api.timing().max_lateness_ms > maxLateness) {
        if (!timingFindings.length) timingFindings.push({late_ms: api.timing().max_lateness_ms,
          message: "host service clock exceeded its budget; timing remains unqualified"});
        // Keep every authored input tick, but never rush through a backlog.
        await api.advance("state", false);
      }
    }
  };
  if (firstTick) await send(firstTick);
  const module = window.director64.module;
  const raw = () => {
    if (!module) return api.capture(); // source-free input-budget test double
    module._d64_render(0, 0, 0);
    const at = module._d64_pixels();
    return {state: api.state(), bytes: module.HEAPU8.slice(at, at + api.width * api.height * 4)};
  };
  const pictures = [raw()];
  for (let i = 1; i < frames; i++) { await send(stride); pictures.push(raw()); }
  if (audioTicks > ticks) await send(audioTicks - ticks);
  const audio = api.takeAudio();
  api.state(); // late audio/video failures must fail the observation
  await window.director64.audio?.suspend();
  // PNG compression and base64 transport are deliberately outside the timed
  // observation. Copying the actual framebuffer above takes no image encode.
  for (const picture of pictures) {
    if (!picture.bytes) continue;
    const bytes = picture.bytes;
    let binary = "";
    for (let i = 0; i < bytes.length; i += 8192)
      binary += String.fromCharCode(...bytes.subarray(i, i + 8192));
    const canvas = document.createElement("canvas");
    canvas.width = api.width; canvas.height = api.height;
    canvas.getContext("2d").putImageData(new ImageData(new Uint8ClampedArray(bytes), api.width, api.height), 0, 0);
    picture.rgba = btoa(binary);
    picture.png = canvas.toDataURL("image/png").split(",")[1];
    delete picture.bytes;
  }
  return {pictures, audio, ticks, timing: api.timing(), timingFindings};
}

function serverFor(site) {
  const root = resolve(site);
  const types = {".html": "text/html", ".js": "text/javascript", ".mjs": "text/javascript", ".json": "application/json", ".wasm": "application/wasm", ".css": "text/css", ".ttf": "font/ttf"};
  return createServer((req, res) => {
    try {
      const name = decodeURIComponent(new URL(req.url, "http://localhost").pathname);
      const path = resolve(root, `.${name === "/" ? "/index.html" : name}`);
      if (!path.startsWith(root + sep) || !statSync(path).isFile()) throw new Error("outside site");
      res.setHeader("Content-Type", types[extname(path)] ?? "application/octet-stream");
      res.end(readFileSync(path));
    } catch { res.statusCode = 404; res.end("not found"); }
  });
}

export async function run(site, plan, output, options = {}) {
  const {chromium} = await import("playwright");
  mkdirSync(output, {recursive: true});
  const log = [], commands = [], checkpoints = [];
  let browser, context, server, state, failure;
  const started = Date.now();
  const report = {schema_version: 1, status: "failed", commands, checkpoints,
    clock: options.fast ? "accelerated-60hz-service" : "realtime-controlled-60hz",
    audible_output_compared: false, original_projector_compared: false, hardware_qualified: false,
    production_pacing_qualified: false};
  report.timing_findings = [];
  const save = () => writeFileSync(join(output, "capture.json"), JSON.stringify(report, null, 2) + "\n");
  try {
    server = serverFor(site);
    await new Promise(resolve => server.listen(0, "127.0.0.1", resolve));
    browser = await chromium.launch({headless: true, executablePath: options.browser ?? undefined,
      args: ["--autoplay-policy=no-user-gesture-required"]});
    context = await browser.newContext({viewport: {width: 1024, height: 768}, deviceScaleFactor: 1,
      locale: "de-DE", timezoneId: "Europe/Berlin", colorScheme: "light"});
    report.environment = {browser: browser.version(), platform: process.platform, arch: process.arch,
      viewport: [1024, 768], device_scale_factor: 1, locale: "de-DE", timezone: "Europe/Berlin"};
    const page = await context.newPage();
    await page.addInitScript(trackDecoders);
    page.on("pageerror", error => { failure ??= String(error); log.push(`ERROR ${error}`); });
    page.on("console", msg => { log.push(`${msg.type()} ${msg.text()}`); });
    page.on("dialog", async dialog => { failure ??= `unexpected dialog: ${dialog.message()}`; await dialog.dismiss(); });
    await page.goto(`http://127.0.0.1:${server.address().port}/?validation=1&seed=${options.seed ?? 42}`, {waitUntil: "load"});
    await page.locator("#start").click({timeout: 120000});
    await page.waitForFunction(() => !!window.director64?.validation, null, {timeout: 120000});
    report.stage = await page.evaluate(() => [director64.validation.width, director64.validation.height]);
    state = await page.evaluate(() => director64.validation.state());
    let artifactAudioHold = false;
    async function releaseArtifacts() {
      if (!artifactAudioHold) return;
      await page.evaluate(async () => {
        await director64.validation.advance("state", false);
        await director64.audio.resume();
      });
      artifactAudioHold = false;
    }
    async function command(text, realtime = !options.fast) {
      if (failure) throw new Error(failure);
      await releaseArtifacts();
      commands.push(text); save(); // preserve an input that hangs or crashes
      const stepped = text.match(/^(step|pad) (\d+) (-?\d+) (-?\d+) (\d+)$/);
      // Yield after at most four service ticks and record excessive lateness.
      // Controlled RPC replay does not qualify the production service loop.
      const chunks = realtime && stepped
        ? Array.from({length: Math.ceil(+stepped[2] / 4)}, (_, i) =>
          `${stepped[1]} ${Math.min(4, +stepped[2] - i * 4)} ${stepped[3]} ${stepped[4]} ${stepped[5]}`)
        : [text];
      for (const chunk of chunks) {
        const reply = await page.evaluate(async ([text, realtime]) => {
          const state = await director64.validation.advance(text, realtime);
          return {state, timing: director64.validation.timing()};
        }, [chunk, realtime]);
        state = reply.state;
        report.service_clock = reply.timing;
        if (realtime && reply.timing.max_lateness_ms > (options.max_lateness_ms ?? 200)) {
          if (!report.timing_findings.length) report.timing_findings.push({command: text,
            late_ms: reply.timing.max_lateness_ms, message: "host service clock exceeded its budget; timing remains unqualified"});
          await page.evaluate(() => director64.validation.advance("state", false));
        }
      }
      if (failure) throw new Error(failure);
      return state;
    }
    function savePicture(label, sample, picture) {
      state = picture.state;
      const base = `${label}-${String(sample).padStart(3, "0")}`;
      writeFileSync(join(output, `${base}.rgba`), Buffer.from(picture.rgba, "base64"));
      writeFileSync(join(output, `${base}.png`), Buffer.from(picture.png, "base64"));
      return {sample, rgba: `${base}.rgba`, png: `${base}.png`, state};
    }
    async function capture(label, sample) {
      if (!artifactAudioHold) {
        await page.evaluate(() => director64.audio.suspend());
        artifactAudioHold = true;
      }
      return savePicture(label, sample, await page.evaluate(() => director64.validation.capture()));
    }
    const [width, height] = report.stage;
    const center = [Math.floor(width / 2), Math.floor(height / 2)];
    const seen = new Set();
    async function stepInput(ticks, x, y, buttons, observation) {
      if (!observation) return command(`step ${ticks} ${x} ${y} ${buttons}`);
      const {label, ...rules} = observation;
      if (!/^[a-z0-9][a-z0-9_-]*$/.test(label ?? "") || seen.has(label))
        throw new Error("unsafe or repeated observation label");
      const count = rules.sequence?.frames ?? 13, stride = rules.sequence?.stride_ticks ?? 5;
      const used = 1 + (count - 1) * stride;
      if (!Number.isInteger(count) || count < 2 || count > 120 || !Number.isInteger(stride) ||
          stride < 1 || stride > 60 || used > ticks || used > 3600)
        throw new Error("observation must fit inside its input tick budget (at most one minute)");
      seen.add(label);
      await releaseArtifacts();
      commands.push("state", `step 1 ${x} ${y} ${buttons}`,
        ...Array(count - 1).fill(`step ${stride} ${x} ${y} ${buttons}`));
      save();
      const window = await page.evaluate(observationWindow, {frames: count, stride, x, y, buttons,
        firstTick: 1, maxLateness: options.max_lateness_ms ?? 200});
      artifactAudioHold = true;
      report.timing_findings.push(...window.timingFindings);
      const sampled = {label, frames: window.pictures.map((frame, i) => savePicture(label, i, frame)),
        observation: rules, input_tick_budget: ticks, observed_ticks: window.ticks};
      const audio = window.audio;
      writeFileSync(join(output, `${label}.f32`), Buffer.from(audio.pcm, "base64"));
      const {pcm: ignored, ...metadata} = audio;
      sampled.audio = {file: `${label}.f32`, ...metadata};
      checkpoints.push(sampled);
      save();
      if (ticks > used) await command(`step ${ticks - used} ${x} ${y} ${buttons}`);
      return state;
    }
    for (const [index, action] of plan.steps.entries()) {
      if (action.when) { try { expectation(state, action.when); } catch { continue; } }
      try {
        if (action.command) {
          const match = action.command.match(/^step (\d+) (-?\d+) (-?\d+) (\d+)$/);
          if (action.observe && !match) throw new Error("observations require idle or pointer step input");
          if (action.observe) await stepInput(+match[1], +match[2], +match[3], +match[4], action.observe);
          else await command(action.command);
        }
        if (action.idle) await stepInput(action.idle, center[0], center[1], 0, action.observe);
        if (action.click || action.sprite) {
          let point = action.click;
          if (!point) {
            const sprite = state.sprites.find(s => s.id === action.sprite);
            if (!sprite) throw new Error(`sprite ${action.sprite} is absent`);
            const [l, t, r, b] = sprite.bounds;
            point = [Math.floor((Math.max(0, l) + Math.min(width, r)) / 2), Math.floor((Math.max(0, t) + Math.min(height, b)) / 2)];
            if (l >= width || r <= 0 || t >= height || b <= 0) throw new Error("sprite has no visible bounds");
          }
          await command(`step 12 ${point[0]} ${point[1]} 0`);
          await command(`step 4 ${point[0]} ${point[1]} 1`);
          await stepInput(action.settle ?? 120, point[0], point[1], 0, action.observe);
        }
        if (action.wait) {
          let reached = false;
          for (let elapsed = 0; elapsed <= (action.ticks ?? 6000); elapsed += 60) {
            try { expectation(state, action.wait); reached = true; break; } catch {}
            if (elapsed + 60 > (action.ticks ?? 6000)) break;
            await command(`step 60 ${center[0]} ${center[1]} 0`);
          }
          if (!reached) expectation(state, action.wait);
        }
        expectation(state, action.expect);
        if (action.label) {
          if (!/^[a-z0-9][a-z0-9_-]*$/.test(action.label) || seen.has(action.label)) throw new Error("unsafe or repeated checkpoint label");
          seen.add(action.label);
          checkpoints.push({label: action.label, frames: [await capture(action.label, 0)],
            observation: plan.observations?.[action.label] ?? {}});
          save();
        }
      } catch (error) { throw new Error(`step ${index + 1} (${action.label ?? "input"}): ${error.message}`, {cause: error}); }
    }
    report.requested_checkpoints = requireCheckpoints(plan, checkpoints);
    // Samples extend the final scene only, so observations cannot perturb an
    // intermediate route or silently change a later checkpoint's inputs.
    const last = checkpoints.at(-1);
    if (last) {
      const observation = last.observation;
      const frames = observation.sequence?.frames ?? 1;
      const stride = observation.sequence?.stride_ticks ?? 5;
      const audioTicks = Math.ceil((observation.audio?.seconds ?? 0) * 60);
      if (frames > 1 || audioTicks) {
        await releaseArtifacts();
        const ticks = Math.max((frames - 1) * stride, audioTicks);
        commands.push("state", ...Array(frames - 1).fill(`step ${stride} ${center[0]} ${center[1]} 0`));
        if (ticks > (frames - 1) * stride) commands.push(`step ${ticks - (frames - 1) * stride} ${center[0]} ${center[1]} 0`);
        save();
        const window = await page.evaluate(observationWindow, {frames, stride, x:center[0], y:center[1],
          audioTicks: ticks, maxLateness: options.max_lateness_ms ?? 200});
        artifactAudioHold = true;
        report.timing_findings.push(...window.timingFindings);
        last.frames = window.pictures.map((frame, i) => savePicture(last.label, i, frame));
        const audio = window.audio;
        writeFileSync(join(output, `${last.label}.f32`), Buffer.from(audio.pcm, "base64"));
        const {pcm: ignored, ...metadata} = audio;
        last.audio = {file: `${last.label}.f32`, ...metadata};
      }
    }
    state = await finalizeBrowser(page);
    if (failure) throw new Error(failure);
    report.service_clock = await page.evaluate(() => director64.validation.timing());
    report.timing_status = report.timing_findings.length ? "failed" : "recorded";
    report.output_scope = options.fast ? "accelerated route; final observation uses realtime service"
      : "realtime service; observed host lateness is recorded separately";
    report.status = "captured";
  } catch (error) {
    report.error = String(error?.stack ?? error);
  } finally {
    report.elapsed_ms = Date.now() - started;
    report.last_state = state;
    writeFileSync(join(output, "browser.log"), log.join("\n") + "\n");
    save();
    await context?.close().catch(() => {});
    await browser?.close().catch(() => {});
    if (server) await new Promise(resolve => server.close(resolve));
  }
  return report;
}

if (process.argv[1] && import.meta.url === pathToFileURL(resolve(process.argv[1])).href) {
  const [site, plan, output, options] = process.argv.slice(2);
  if (!site || !plan || !output) throw new Error("browser.mjs SITE PLAN OUTPUT [OPTIONS.json]");
  const result = await run(site, JSON.parse(readFileSync(plan)), output, options ? JSON.parse(readFileSync(options)) : {});
  process.stdout.write(JSON.stringify({status: result.status, checkpoints: result.checkpoints.length, error: result.error}) + "\n");
  process.exitCode = result.status === "captured" ? 0 : 1;
}
