// A worker of the importer's pool (bundled to pool.js by build.mjs): runs
// parse, movie, prescale, sound and video jobs (jobs.mjs) for the import worker.
import {serve} from "./jobs.mjs";
import {encodeAudio} from "./opus.mjs";
import {encodeVideo} from "./vp8.mjs";
import {DirectorFile} from "projectorrays/web";

serve({
  receive: (handler) => { self.onmessage = ({data}) => handler(data); },
  send: (message, transfer) => self.postMessage(message, transfer ?? []),
}, {
  // A classic worker runs the parser glue with importScripts.
  loadParser: async ({glueUrl, wasmBinary}) => {
    await DirectorFile.loadModule({glueUrl, wasmBinary, useScriptTag: true});
    return DirectorFile;
  },
  encodeAudio: typeof AudioEncoder === "function" ? encodeAudio : null,
  encodeVideo: typeof VideoEncoder === "function" ? encodeVideo : null,
});
