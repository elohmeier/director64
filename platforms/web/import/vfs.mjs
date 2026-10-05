// The importer's in-memory file system: the extracted disc and every stage's
// output live here, under POSIX paths, for the length of one import. A lazy
// file holds only its size and a loader: the disc's contents are read from
// the image when a stage asks, so files no stage reads (screensavers,
// installers) never occupy memory.
export class Vfs {
  constructor() {
    this.files = new Map();
    this.dirs = new Set(["/"]);
  }
  static normalize(path) {
    const parts = [];
    for (const part of path.split("/")) {
      if (!part || part === ".") continue;
      if (part === "..") parts.pop();
      else parts.push(part);
    }
    return "/" + parts.join("/");
  }
  parent(path) {
    return path.slice(0, path.lastIndexOf("/")) || "/";
  }
  error(code, path, syscall) {
    return Object.assign(new Error(`${code}: no such file or directory, ${syscall} '${path}'`), {code, path, syscall});
  }
  mkdir(path, recursive = false) {
    path = Vfs.normalize(path);
    if (this.dirs.has(path)) return;
    if (this.files.has(path)) throw Object.assign(new Error(`EEXIST: ${path}`), {code: "EEXIST"});
    const parent = this.parent(path);
    if (!this.dirs.has(parent)) {
      if (!recursive) throw this.error("ENOENT", path, "mkdir");
      this.mkdir(parent, true);
    }
    this.dirs.add(path);
  }
  write(path, bytes) {
    path = Vfs.normalize(path);
    if (!this.dirs.has(this.parent(path))) throw this.error("ENOENT", path, "open");
    if (this.dirs.has(path)) throw Object.assign(new Error(`EISDIR: ${path}`), {code: "EISDIR"});
    this.files.set(path, bytes);
  }
  // A file whose bytes come from load() whenever it is read, never kept.
  writeLazy(path, size, load) {
    path = Vfs.normalize(path);
    if (!this.dirs.has(this.parent(path))) throw this.error("ENOENT", path, "open");
    this.files.set(path, {size, load});
  }
  read(path) {
    const entry = this.files.get(Vfs.normalize(path));
    if (!entry) throw this.error("ENOENT", path, "open");
    return entry instanceof Uint8Array ? entry : entry.load();
  }
  size(path) {
    const entry = this.files.get(Vfs.normalize(path));
    if (!entry) throw this.error("ENOENT", path, "stat");
    return entry instanceof Uint8Array ? entry.length : entry.size;
  }
  isFile(path) {
    return this.files.has(Vfs.normalize(path));
  }
  exists(path) {
    path = Vfs.normalize(path);
    return this.files.has(path) || this.dirs.has(path);
  }
  list(path) {
    path = Vfs.normalize(path);
    if (!this.dirs.has(path)) throw this.error("ENOENT", path, "scandir");
    const prefix = path === "/" ? "/" : path + "/";
    const names = new Map();
    for (const dir of this.dirs)
      if (dir !== path && dir.startsWith(prefix) && !dir.slice(prefix.length).includes("/"))
        names.set(dir.slice(prefix.length), "dir");
    for (const file of this.files.keys())
      if (file.startsWith(prefix) && !file.slice(prefix.length).includes("/"))
        names.set(file.slice(prefix.length), "file");
    // Sorted, where a disk would give its own order: callers that care sort.
    return [...names].sort(([a], [b]) => (a < b ? -1 : a > b ? 1 : 0));
  }
  remove(path) {
    path = Vfs.normalize(path);
    this.files.delete(path);
    for (const file of [...this.files.keys()]) if (file.startsWith(path + "/")) this.files.delete(file);
    for (const dir of [...this.dirs]) if (dir === path || dir.startsWith(path + "/")) this.dirs.delete(dir);
  }
  // Bytes held in memory; lazy files count nothing.
  bytes() {
    let total = 0;
    for (const data of this.files.values())
      if (data instanceof Uint8Array) total += data.length;
    return total;
  }
}
