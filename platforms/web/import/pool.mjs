// A fixed set of workers running the importer's jobs (jobs.mjs), one job per
// worker at a time. The pipeline streams work through `map`: inputs are made
// only when a worker is free and results are consumed as they arrive, so a
// stage never holds every input and output at once.
export class Pool {
  // endpoints: [{receive(handler), send(message, transfer), close()}], each a
  // worker running jobs.mjs `serve`; init: the `init` message every worker gets.
  constructor(endpoints, init) {
    this.endpoints = endpoints;
    this.size = endpoints.length;
    this.pending = new Map();
    this.nextId = 0;
    this.ready = Promise.all(endpoints.map((endpoint) => new Promise((resolve) => {
      endpoint.receive((message) => {
        if (message.type === "ready") return resolve();
        const job = this.pending.get(message.id);
        // A job's questions about the import's files (movie jobs).
        if (message.type === "need") {
          const {answers, transfer} = job.provide(message.requests);
          endpoint.send({type: "provide", id: message.id, answers}, transfer);
          return;
        }
        this.pending.delete(message.id);
        if ("error" in message) job.reject(new Error(message.error));
        else job.resolve(message.result);
      });
      endpoint.send(init);
    })));
  }

  // Runs `kind` on count inputs: input(i) makes the i-th payload (and returns
  // {payload, transfer} or the payload alone), consume(i, result) takes its
  // result, provide(requests) answers a job's questions about the import's
  // files. Resolves when every job has been consumed; the first failure
  // rejects after the jobs in flight settle.
  async map(kind, count, input, consume, {signal, onProgress, provide} = {}) {
    await this.ready;
    let next = 0, done = 0, failure = null;
    const lane = async (endpoint) => {
      while (next < count && !failure) {
        if (signal?.aborted) {
          failure ??= Object.assign(new Error("import cancelled"), {name: "AbortError"});
          return;
        }
        const i = next++;
        const made = input(i);
        const {payload, transfer} = made?.payload !== undefined ? made : {payload: made, transfer: []};
        const id = this.nextId++;
        const result = await new Promise((resolve, reject) => {
          this.pending.set(id, {resolve, reject, provide});
          endpoint.send({type: "job", id, kind, payload}, transfer ?? []);
        }).catch((error) => {
          failure ??= error;
        });
        if (failure) return;
        consume(i, result);
        onProgress?.(++done / count);
      }
    };
    await Promise.all(this.endpoints.map(lane));
    if (failure) throw failure;
  }

  close() {
    for (const endpoint of this.endpoints) endpoint.close();
  }
}
