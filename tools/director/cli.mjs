// Whether a module runs as the command line entry. The converter's stages
// are functions the browser importer calls; each keeps its CLI for the
// native pipeline, run only when Node started that file.
import path from "node:path";
import {pathToFileURL} from "node:url";

export function isMain(url) {
  const entry = globalThis.process?.argv?.[1];
  return !!entry && url === pathToFileURL(path.resolve(entry)).href;
}
