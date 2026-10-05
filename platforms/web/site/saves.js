// Durable saves in the origin-private file system. The runtime's save device
// is the console's 128 KiB FlashRAM image; every committed generation is
// written here whole, and reported saved only once that write has closed.
// Saves live under their own namespace, apart from any converted-asset cache,
// so clearing or rebuilding a cache never touches them.

export const FLASH_BYTES = 131072;

// An archive slot starts with its magic, or the slot is erased flash.
export function plausibleFlash(bytes) {
  if (bytes.length !== FLASH_BYTES) return false;
  const half = FLASH_BYTES / 2;
  for (const offset of [0, half]) {
    const slot = bytes.subarray(offset, offset + half);
    const magic = String.fromCharCode(...slot.subarray(0, 4));
    if (!/^[A-Z0-9]{4}$/.test(magic) && !slot.every((b) => b === 255)) return false;
  }
  return true;
}

export class SaveStore extends EventTarget {
  constructor(namespace) {
    super();
    this.parts = namespace.split("/").filter(Boolean);
    this.directory = null;
    this.pending = null;
    this.writing = false;
    this.state = {kind: "idle"};
  }
  async open() {
    try {
      let directory = await navigator.storage.getDirectory();
      for (const part of [...this.parts, "saves"]) {
        directory = await directory.getDirectoryHandle(part, {create: true});
      }
      this.directory = directory;
      // Best effort: persistent storage is not evicted under pressure.
      navigator.storage.persist?.().catch(() => {});
    } catch (error) {
      this.report({kind: "unavailable", message: String(error)});
    }
    return this.directory !== null;
  }
  async read(name = "flash.bin") {
    if (!this.directory) return null;
    try {
      const file = await (await this.directory.getFileHandle(name)).getFile();
      const bytes = new Uint8Array(await file.arrayBuffer());
      return bytes.length === FLASH_BYTES ? bytes : null;
    } catch (error) {
      if (error.name === "NotFoundError") return null;
      throw error;
    }
  }
  async write(name, bytes) {
    // createWritable writes a swap file that replaces the original on close,
    // so an interrupted write leaves the previous generation intact.
    const handle = await this.directory.getFileHandle(name, {create: true});
    const writable = await handle.createWritable();
    try {
      await writable.write(bytes);
      await writable.close();
    } catch (error) {
      await writable.abort().catch(() => {});
      throw error;
    }
  }
  report(state) {
    this.state = state;
    this.dispatchEvent(new CustomEvent("state", {detail: state}));
  }
  get busy() { return this.writing || this.pending !== null; }
  // A newer generation replaces one still waiting; writes never overlap.
  commit(bytes) {
    if (!this.directory) {
      this.report({kind: "unavailable", message: "this browser keeps no durable storage"});
      return;
    }
    this.pending = bytes;
    this.report({kind: "saving"});
    if (!this.writing) this.drain();
  }
  async drain() {
    this.writing = true;
    while (this.pending) {
      const bytes = this.pending;
      this.pending = null;
      try {
        await this.write("flash.bin", bytes);
        if (!this.pending) this.report({kind: "saved", at: new Date()});
      } catch (error) {
        this.report({kind: "failed", message: error.name === "QuotaExceededError"
          ? "storage quota exceeded" : String(error.message || error)});
      }
    }
    this.writing = false;
  }
  // Import keeps the previous save beside the new one until it has booted.
  async replace(bytes) {
    const current = await this.read();
    if (current) await this.write("flash.bak", current);
    await this.write("flash.bin", bytes);
  }
  async restoreBackup() {
    const backup = await this.read("flash.bak");
    if (!backup) return false;
    await this.write("flash.bin", backup);
    return true;
  }
  async discardBackup() {
    await this.directory?.removeEntry("flash.bak").catch(() => {});
  }
}
