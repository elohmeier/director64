// Director 5 digital video (platforms/n64/d5_video.c on the console): one
// player per sprite channel showing a video member. The runtime tells each
// player its time every service step; the player decodes, with the browser's
// WebCodecs, up to the frame for that time and keeps it for the compositor,
// and plays the movie's sound in step with it.
//
// A video asset is the importer's container: a u32 header length, a JSON
// header {codec, width, height, frames: [[microseconds, key, offset, size]]}
// and the encoded chunks.
export function parseVideo(bytes) {
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint32(0, true);
  const header = JSON.parse(new TextDecoder().decode(bytes.subarray(4, 4 + length)));
  return {...header, data: bytes.subarray(4 + length)};
}

// The last frame at or before `microseconds`.
export function frameAt(frames, microseconds) {
  let lo = 0, hi = frames.length - 1, found = -1;
  while (lo <= hi) {
    const mid = (lo + hi) >> 1;
    if (frames[mid][0] <= microseconds) {
      found = mid;
      lo = mid + 1;
    } else hi = mid - 1;
  }
  return found;
}

class Player {
  constructor(video, onFrame) {
    this.video = video;
    this.onFrame = onFrame;
    this.canvas = new OffscreenCanvas(video.width || 1, video.height || 1);
    this.context = this.canvas.getContext("2d", {willReadFrequently: true});
    this.fed = -1; // the last chunk given to the decoder
    this.wanted = -1; // the frame the runtime's time asks for
    this.pixels = null;
    this.decoder = new VideoDecoder({
      output: (frame) => this.output(frame),
      error: (error) => console.warn("video decoder", error),
    });
    this.decoder.configure(this.config());
  }
  output(frame) {
    // Only the frame the runtime wants is drawn; frames decoded on the way
    // there are dropped unread.
    if (this.wanted >= 0 && frame.timestamp === this.video.frames[this.wanted][0]) {
      // A console stream's header leaves the size to its first frame.
      if (!this.video.width) {
        this.video.width = this.canvas.width = frame.displayWidth;
        this.video.height = this.canvas.height = frame.displayHeight;
      }
      this.context.drawImage(frame, 0, 0);
      this.pixels = this.context.getImageData(0, 0, this.video.width, this.video.height).data;
      this.onFrame();
    }
    frame.close();
  }
  config() {
    const {codec, width, height} = this.video;
    return width ? {codec, codedWidth: width, codedHeight: height} : {codec};
  }
  chunk(index) {
    const [timestamp, key, offset, size] = this.video.frames[index];
    return new EncodedVideoChunk({type: key ? "key" : "delta", timestamp,
      data: this.video.data.subarray(offset, offset + size)});
  }
  seek(time600) {
    const target = frameAt(this.video.frames, Math.round(time600 * 1e6 / 600));
    if (target < 0 || target === this.wanted || this.decoder.state === "closed") return;
    this.wanted = target;
    // Far jumps and rewinds restart from the keyframe before the target;
    // near ones decode forward from where the decoder is.
    if (target < this.fed || target > this.fed + 30) {
      let key = target;
      while (key > 0 && !this.video.frames[key][1]) key--;
      this.decoder.reset();
      this.decoder.configure(this.config());
      this.fed = key - 1;
    }
    while (this.fed < target) this.decoder.decode(this.chunk(++this.fed));
  }
  close() {
    if (this.decoder.state !== "closed") this.decoder.close();
  }
}

export class VideoPlayers {
  // lookup(asset) gives a video asset's bytes; audio is the AudioEngine.
  constructor(lookup, audio) {
    this.lookup = lookup;
    this.audio = audio;
    this.players = new Map();
    this.revision = 0;
  }
  // The runtime's per-step report for one sprite (web_runtime.c update_videos).
  update(sprite, asset, sound, time600, rate, serial, gain) {
    let entry = this.players.get(sprite);
    if (asset === null) {
      if (entry) this.close(sprite);
      return;
    }
    if (!entry) {
      const bytes = asset ? this.lookup(asset) : null;
      let player = null;
      if (bytes && typeof VideoDecoder !== "undefined") {
        try {
          player = new Player(parseVideo(bytes), () => this.revision++);
        } catch (error) {
          console.warn(`video ${asset}`, error);
        }
      } else if (asset) console.warn(`missing video ${asset}`);
      entry = {player, sound, serial: -1, playing: false};
      this.players.set(sprite, entry);
    }
    entry.player?.seek(time600);
    // The movie's sound: started (or re-seeked) when it plays and the time
    // jumped, stopped when it stops, pitched by the rate, as the console's.
    if (entry.sound) {
      const play = rate > 0 && time600 / 600 < this.audio.duration(entry.sound);
      if (play && (!entry.playing || entry.serial !== serial))
        this.audio.playVideo(sprite, entry.sound, time600 / 600);
      else if (!play && entry.playing)
        this.audio.stopVideo(sprite);
      if (play) this.audio.videoRate(sprite, rate, gain);
      entry.playing = play;
    }
    entry.serial = serial;
  }
  frame(sprite) {
    const player = this.players.get(sprite)?.player;
    return player?.pixels ? {pixels: player.pixels, width: player.video.width,
      height: player.video.height} : null;
  }
  close(sprite) {
    const entry = this.players.get(sprite);
    entry?.player?.close();
    if (entry?.sound) this.audio.stopVideo(sprite);
    this.players.delete(sprite);
    this.revision++;
  }
  closeAll() {
    for (const sprite of [...this.players.keys()]) this.close(sprite);
  }
}
