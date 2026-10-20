// Optional rendered-probe measurements. Each session gets its own file, so
// journeys that reboot or start several probes retain every sample.
import {appendFileSync, mkdirSync} from "node:fs";
import {join} from "node:path";
import {performance} from "node:perf_hooks";

export function probeMetrics(directory = process.env.DIRECTOR64_PROBE_METRICS) {
  if (!directory) return {start() {}, end() {}};
  mkdirSync(directory, {recursive: true});
  const file = join(directory, `${process.pid}.jsonl`);
  let index = 0;
  return {
    start(command) {
      // Write the attempted input before executing it: a killed or hung
      // runtime must retain the command that never produced a sample.
      if (command !== undefined)
        appendFileSync(join(directory, `${process.pid}.commands.txt`), command + "\n");
      return {time: performance.now(), cpu: process.cpuUsage()};
    },
    end(start, command, module) {
      const elapsed = performance.now() - start.time;
      const cpu = process.cpuUsage(start.cpu);
      appendFileSync(file, JSON.stringify({
        index: index++, command, elapsed_ms: elapsed,
        cpu_ms: (cpu.user + cpu.system) / 1000,
        rss_bytes: process.memoryUsage().rss,
        peak_rss_bytes: process.resourceUsage().maxRSS * 1024,
        wasm_bytes: module.HEAPU8.byteLength,
        movie: module._d64_movie ? module.UTF8ToString(module._d64_movie()) : "",
        frame: module._d64_frame?.() ?? 0,
        script_errors: module._d64_script_errors?.() ?? 0,
      }) + "\n");
    },
  };
}
