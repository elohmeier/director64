// Converted games in the origin-private file system, so a disc is imported
// once. An entry is keyed by the converter, runtime ABI and edition that made
// it and is committed by writing its manifest last: a directory without a
// valid manifest is an interrupted import and is removed, never loaded.
// Entries live apart from saves (saves.js); removing one never touches them.
// A game's asset packs, each a blob and its index: images, sounds, the fonts
// the page sets text in, playFile streams, shipped data files, digital video
// and print documents.
export const PACKS = ["images", "audio", "fonts", "streams", "data", "video", "print"];
const FILES = ["game.d64p", ...PACKS.flatMap((kind) => [`${kind}.bin`, `${kind}.json`])];
const EMPTY = {blob: new Uint8Array(), index: {}};
const encoder = new TextEncoder(), decoder = new TextDecoder();

async function readFile(directory, name) {
  const file = await (await directory.getFileHandle(name)).getFile();
  return new Uint8Array(await file.arrayBuffer());
}
async function writeFile(directory, name, bytes) {
  const handle = await directory.getFileHandle(name, {create: true});
  const writable = await handle.createWritable();
  try {
    await writable.write(bytes);
    await writable.close();
  } catch (error) {
    await writable.abort().catch(() => {});
    throw error;
  }
}

export class PackageCache {
  constructor(slug, key) {
    this.slug = slug;
    this.key = key;
    this.root = null;
  }
  async open() {
    try {
      let directory = await navigator.storage.getDirectory();
      for (const part of ["director64", this.slug, "packages"])
        directory = await directory.getDirectoryHandle(part, {create: true});
      this.root = directory;
    } catch {
      this.root = null;
    }
    return this.root !== null;
  }
  // Entries made by another converter or edition, and interrupted imports.
  async prune() {
    if (!this.root) return;
    for await (const [name, handle] of this.root.entries()) {
      if (handle.kind !== "directory") continue;
      if (name !== this.key || !(await this.manifest(handle)))
        await this.root.removeEntry(name, {recursive: true});
    }
  }
  async manifest(directory) {
    try {
      const manifest = JSON.parse(decoder.decode(await readFile(directory, "manifest.json")));
      if (manifest.key !== this.key) return null;
      for (const name of FILES) {
        const file = await (await directory.getFileHandle(name)).getFile();
        if (file.size !== manifest.bytes[name]) return null;
      }
      return manifest;
    } catch {
      return null;
    }
  }
  async load() {
    if (!this.root) return null;
    let directory;
    try {
      directory = await this.root.getDirectoryHandle(this.key);
    } catch {
      return null;
    }
    const manifest = await this.manifest(directory);
    if (!manifest) return null;
    const [gamePackage, ...files] = await Promise.all(FILES.map((name) => readFile(directory, name)));
    return {
      manifest,
      package: gamePackage,
      ...Object.fromEntries(PACKS.map((kind, i) =>
        [kind, {blob: files[2 * i], index: JSON.parse(decoder.decode(files[2 * i + 1]))}])),
    };
  }
  async store(game, report) {
    if (!this.root) throw new Error("this browser keeps no durable storage");
    await this.root.removeEntry(this.key, {recursive: true}).catch(() => {});
    const directory = await this.root.getDirectoryHandle(this.key, {create: true});
    const contents = {"game.d64p": game.package};
    for (const kind of PACKS) {
      const {blob, index} = game[kind] ?? EMPTY;
      contents[`${kind}.bin`] = blob;
      contents[`${kind}.json`] = encoder.encode(JSON.stringify(index));
    }
    try {
      for (const name of FILES) await writeFile(directory, name, contents[name]);
      const bytes = Object.fromEntries(FILES.map((name) => [name, contents[name].length]));
      await writeFile(directory, "manifest.json",
        encoder.encode(JSON.stringify({key: this.key, created: new Date().toISOString(), bytes, report})));
    } catch (error) {
      await this.root.removeEntry(this.key, {recursive: true}).catch(() => {});
      throw error;
    }
  }
  async remove() {
    await this.root?.removeEntry(this.key, {recursive: true}).catch(() => {});
  }
  async usage() {
    const estimate = await navigator.storage.estimate?.();
    return estimate ?? null;
  }
}
