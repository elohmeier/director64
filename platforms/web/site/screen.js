// The stage on the display: presenting frames at either scaling, fullscreen,
// and keeping the screen awake while a game runs.
export const WIDTH = 640, HEIGHT = 480;
const MAX_FACTOR = 8;

// The integer upscale "sharp" scaling draws at: the display scale rounded up,
// so the browser only ever shrinks the nearest-neighbour image a little.
export function sharpFactor(cssWidth, cssHeight, pixelRatio = 1, width = WIDTH, height = HEIGHT) {
  const scale = Math.min(cssWidth / width, cssHeight / height) * pixelRatio;
  return Math.max(1, Math.min(MAX_FACTOR, Math.ceil(scale - 1e-6)));
}

// Presents the runtime's RGBA frames: 640x480, or an 800x600 game's
// unscaled stage (setSize). "smooth" lets the browser
// filter the stage to the display size; "sharp" first enlarges it by whole
// pixels so they stay crisp, then filters only the remaining fraction.
export class Presenter {
  constructor(canvas, scaling = "sharp") {
    this.canvas = canvas;
    this.context = canvas.getContext("2d", {alpha: false});
    this.source = document.createElement("canvas");
    this.sourceContext = this.source.getContext("2d", {alpha: false});
    this.width = this.height = 0;
    this.setSize(WIDTH, HEIGHT);
    this.scaling = scaling;
    this.factor = 1;
    new ResizeObserver(() => this.resize()).observe(canvas);
    this.resize();
  }
  setSize(width, height) {
    if (width === this.width && height === this.height) return;
    this.width = this.source.width = width;
    this.height = this.source.height = height;
    this.frame = this.sourceContext.createImageData(width, height);
    if (this.canvas.width) this.resize(true);
  }
  setScaling(scaling) {
    this.scaling = scaling;
    this.resize(true);
  }
  resize(force = false) {
    const rect = this.canvas.getBoundingClientRect();
    const factor = this.scaling === "sharp" && rect.width
      ? sharpFactor(rect.width, rect.height, devicePixelRatio || 1, this.width, this.height) : 1;
    if (!force && factor === this.factor && this.canvas.width === this.width * factor) return;
    this.factor = factor;
    this.canvas.width = this.width * factor;
    this.canvas.height = this.height * factor;
    this.draw();
  }
  // `pixels` is the runtime's framebuffer, width*height*4 bytes.
  present(pixels) {
    this.frame.data.set(pixels);
    this.sourceContext.putImageData(this.frame, 0, 0);
    this.draw();
  }
  draw() {
    this.context.imageSmoothingEnabled = this.factor === 1;
    this.context.drawImage(this.source, 0, 0, this.canvas.width, this.canvas.height);
  }
}

// Fullscreen of the stage. With a keyboard lock, Escape reaches the game
// (several read it) and holding it leaves fullscreen.
export async function toggleFullscreen(stage) {
  if (document.fullscreenElement) {
    await document.exitFullscreen();
    return false;
  }
  await stage.requestFullscreen({navigationUI: "hide"});
  await navigator.keyboard?.lock?.(["Escape"]).catch(() => {});
  return true;
}
globalThis.document?.addEventListener("fullscreenchange", () => {
  if (!document.fullscreenElement) navigator.keyboard?.unlock?.();
});

// Holds a screen wake lock while wanted. A desktop does not count controller
// input as activity and would otherwise dim mid-game; the lock lapses while
// the page is hidden and is taken again when it shows.
export class WakeLock {
  constructor() {
    this.wanted = false;
    this.sentinel = null;
    this.requesting = false;
    document.addEventListener("visibilitychange", () => this.update());
  }
  want(on) {
    this.wanted = on;
    this.update();
  }
  async update() {
    if (this.requesting) return;
    if (this.wanted && !document.hidden && !this.sentinel && navigator.wakeLock) {
      this.requesting = true;
      try {
        const sentinel = await navigator.wakeLock.request("screen");
        sentinel.addEventListener("release", () => {
          if (this.sentinel === sentinel) this.sentinel = null;
        });
        this.sentinel = sentinel;
      } catch {
        this.sentinel = null;
      } finally {
        this.requesting = false;
      }
      if (!this.wanted) this.update();
    } else if (!this.wanted && this.sentinel) {
      const sentinel = this.sentinel;
      this.sentinel = null;
      await sentinel.release().catch(() => {});
    }
  }
}
