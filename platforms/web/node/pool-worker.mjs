// A worker of the importer's pool under Node (the parity harness,
// platforms/web/import/parity.mjs): the browser's pool-worker.mjs over
// worker_threads, with the parser's Node entry and no sound encoder.
import {parentPort} from "node:worker_threads";
import {serve} from "../import/jobs.mjs";

serve({
  receive: (handler) => parentPort.on("message", handler),
  send: (message, transfer) => parentPort.postMessage(message, transfer ?? []),
}, {
  loadParser: async () => (await import("../../../node_modules/projectorrays/dist/pkg/node.es.js")).DirectorFile,
  encodeAudio: null,
});
