// A worker of the exporter's import pool: the Node pool worker
// (platforms/web/node/pool-worker.mjs) with FFmpeg as the video and long
// sounds' encoder (video.mjs); short sounds stay PCM WAV. Images go to
// lossless WebP (webp.mjs).
import {parentPort} from "node:worker_threads";
import {serve} from "../../web/import/jobs.mjs";
import {encodeAudio, encodeVideo} from "./video.mjs";
import {encodeImage} from "./webp.mjs";

serve({
  receive: (handler) => parentPort.on("message", handler),
  send: (message, transfer) => parentPort.postMessage(message, transfer ?? []),
}, {
  loadParser: async () => (await import("../../../node_modules/projectorrays/dist/pkg/node.es.js")).DirectorFile,
  encodeAudio,
  encodeImage,
  encodeVideo,
});
