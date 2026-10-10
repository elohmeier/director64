// A worker of the exporter's import pool: the Node pool worker
// (platforms/web/node/pool-worker.mjs) with FFmpeg as the video encoder.
// Sounds stay PCM WAV, which the TV player mixes directly.
import {parentPort} from "node:worker_threads";
import {serve} from "../../web/import/jobs.mjs";
import {encodeVideo} from "./video.mjs";

serve({
  receive: (handler) => parentPort.on("message", handler),
  send: (message, transfer) => parentPort.postMessage(message, transfer ?? []),
}, {
  loadParser: async () => (await import("../../../node_modules/projectorrays/dist/pkg/node.es.js")).DirectorFile,
  encodeAudio: null,
  encodeVideo,
});
