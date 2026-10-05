// The importer's independent per-item work: parse one Director file, convert
// one movie, prescale one image, encode one sound or one linked movie's video. The pipeline maps these
// over a worker pool (pool.mjs, one converter and parser per worker) or,
// without one, runs them in its own thread with the same code. A job's result
// depends only on its input, so the order the pool finishes in never changes
// the output.
import {dumpDirector} from "../../../tools/director/dump-format.mjs";
import {loadConverter} from "./convert.mjs";
import {Vfs} from "./vfs.mjs";

// A pool worker's view of the import's files for one movie: what the movie
// writes stays here, everything else is asked of the import worker. A
// question not answered yet reads as absent and is recorded; the job then
// fetches the answers and runs again, so a result is only ever made from
// complete answers (convertMovie).
export class ProxyFiles {
  constructor(outputs, answers) {
    this.local = new Vfs();
    this.outputs = outputs.map((path) => Vfs.normalize(path));
    this.answers = answers;
    this.missing = new Map();
  }
  own(path) {
    path = Vfs.normalize(path);
    return this.local.isFile(path) || this.outputs.some((o) => path === o || path.startsWith(o + "/"));
  }
  ask(op, path) {
    const key = `${op} ${Vfs.normalize(path)}`;
    const answer = this.answers.get(key);
    if (answer === undefined) this.missing.set(key, {op, path: Vfs.normalize(path)});
    return answer ?? {absent: true};
  }
  absent(path) {
    return Object.assign(new Error(`ENOENT: ${path}`), {code: "ENOENT"});
  }
  read(path) {
    if (this.own(path)) return this.local.read(path);
    const answer = this.ask("read", path);
    if (answer.absent) throw this.absent(path);
    return answer.data;
  }
  size(path) {
    if (this.own(path)) return this.local.size(path);
    const answer = this.ask("size", path);
    if (answer.absent) throw this.absent(path);
    return answer.value;
  }
  exists(path) {
    return this.own(path) ? this.local.exists(path) : !!this.ask("exists", path).value;
  }
  isFile(path) {
    return this.own(path) ? this.local.isFile(path) : !!this.ask("isFile", path).value;
  }
  list(path) {
    if (this.own(path)) return this.local.list(path);
    const answer = this.ask("list", path);
    if (answer.absent) throw this.absent(path);
    return answer.entries;
  }
  write(path, bytes) {
    this.local.mkdir(this.local.parent(Vfs.normalize(path)), true);
    this.local.write(path, bytes);
  }
  mkdir(path) {
    this.local.mkdir(path, true);
  }
  remove(path) {
    this.local.remove(path);
  }
  // Everything the movie wrote, in the order it wrote it.
  written() {
    return [...this.local.files];
  }
}

// The import worker's answers to a pool worker's questions about its files.
export function answer(vfs, requests) {
  const answers = [], transfer = [];
  for (const {op, path} of requests) {
    let value;
    try {
      switch (op) {
        case "read": {
          const data = vfs.read(path).slice();
          transfer.push(data.buffer);
          value = {data};
          break;
        }
        case "size": value = {value: vfs.size(path)}; break;
        case "exists": value = {value: vfs.exists(path)}; break;
        case "isFile": value = {value: vfs.isFile(path)}; break;
        case "list": value = {entries: vfs.list(path)}; break;
        default: throw new Error(`unknown file question ${op}`);
      }
    } catch (error) {
      if (error?.code !== "ENOENT") throw error;
      value = {absent: true};
    }
    answers.push([`${op} ${path}`, value]);
  }
  return {answers, transfer};
}

// One movie of the compile stage (compile-movie): its record (JSON bytes) and
// the files it wrote. fetch(requests) brings answers to the questions it
// could not answer yet; `answers` keeps them across this worker's jobs.
async function convertMovie({request, index, answers: sent = []}, tools, fetch, answers) {
  for (const [key, value] of sent) answers.set(key, value);
  for (;;) {
    const files = new ProxyFiles([request.output], answers);
    let record, failure = null;
    try {
      record = tools.converter.directorBytes({...request, stage: "compile-movie", index}, files);
    } catch (error) {
      failure = error;
    }
    if (files.missing.size) {
      for (const [key, value] of await fetch([...files.missing.values()])) answers.set(key, value);
      continue;
    }
    if (failure) throw failure;
    return {record, files: files.written()};
  }
}

// tools: {converter, DirectorFile, encodeAudio, encodeVideo}; context (pool workers):
// {fetch, answers} for movie jobs.
export async function runJob(kind, payload, tools, context) {
  switch (kind) {
    case "parse": {
      const file = await tools.DirectorFile.read(payload);
      try {
        return dumpDirector(file);
      } finally {
        file.destroy();
      }
    }
    case "movie":
      return convertMovie(payload, tools, context.fetch, context.answers);
    case "prescale":
      return tools.converter.prescale(payload);
    case "sound":
      return tools.encodeAudio(payload);
    case "video": {
      const frames = tools.converter.frames(payload);
      try {
        return await tools.encodeVideo(frames);
      } finally {
        frames.close();
      }
    }
    default:
      throw new Error(`unknown import job ${kind}`);
  }
}

// The transferable buffers of a job's result.
export function transferables(result) {
  if (result instanceof Uint8Array) return [result.buffer];
  if (result?.stored instanceof Uint8Array) return [result.stored.buffer];
  if (result?.record instanceof Uint8Array)
    return [result.record.buffer, ...result.files.flatMap(([, data]) => (data instanceof Uint8Array ? [data.buffer] : []))];
  return [];
}

// A pool worker's side: `init` brings the compiled converter module and the
// parser's files; each `job` replies with its result or error under its id;
// a movie job's questions go out as `need` and come back as `provide`.
//   endpoint: {receive(handler), send(message, transfer)}
//   loadParser(parser): the environment's ProjectorRays, loaded once
//   encodeAudio(converter, wav): the environment's sound encoder, if any
//   encodeVideo(frames): the environment's video encoder, if any
export function serve(endpoint, {loadParser, encodeAudio, encodeVideo}) {
  let tools = null;
  let parser = null;
  // A movie batch's answers, kept for its later jobs on this worker.
  let batch = null, answers = new Map();
  const waiting = new Map();
  endpoint.receive(async (message) => {
    if (message.type === "init") {
      const converter = await loadConverter(message.converter);
      tools = {converter, encodeAudio: encodeAudio && ((wav) => encodeAudio(converter, wav)), encodeVideo};
      parser = message.parser;
      endpoint.send({type: "ready"});
      return;
    }
    if (message.type === "provide") {
      waiting.get(message.id)(message.answers);
      waiting.delete(message.id);
      return;
    }
    const {id, kind, payload} = message;
    try {
      if (kind === "parse" && !tools.DirectorFile) tools.DirectorFile = await loadParser(parser);
      if (kind === "movie" && payload.batch !== batch) {
        batch = payload.batch;
        answers = new Map();
      }
      const fetch = (requests) => new Promise((resolve) => {
        waiting.set(id, resolve);
        endpoint.send({type: "need", id, requests});
      });
      const result = await runJob(kind, payload, tools, {fetch, answers});
      endpoint.send({id, result}, transferables(result));
    } catch (error) {
      endpoint.send({id, error: String(error?.message ?? error)});
    }
  });
}
