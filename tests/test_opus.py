"""Real FFmpeg Opus exports retain sample bounds across C and browser loaders."""
import json
import shutil
import struct
import subprocess
from pathlib import Path

import pytest

from director64.convert import opus

ROOT = Path(__file__).resolve().parents[1]
pytestmark = pytest.mark.skipif(not shutil.which('ffmpeg'), reason='needs ffmpeg')


@pytest.fixture(scope='module')
def source_tool(tmp_path_factory):
    tool = tmp_path_factory.mktemp('opus') / 'source-tool'
    subprocess.run(['cc', '-std=c17', '-Wall', '-Wextra', '-Werror', '-pedantic',
                    '-Itests/native/android_stubs', '-ffunction-sections', '-fdata-sections',
                    '-Wl,--gc-sections', 'tests/native/opus_source_tool.c', '-o', tool],
                   cwd=ROOT, check=True)
    return tool


@pytest.mark.parametrize(('rate', 'frames'), [
    (8000, 0), (8000, 80), (11025, 1234), (22050, 7789), (44100, 45123),
])
def test_opus_source_grid_survives_resampling(tmp_path, source_tool, rate, frames):
    data = bytes(frames * 2)
    wav = (b'RIFF' + struct.pack('<I', 36 + len(data)) + b'WAVEfmt '
           + struct.pack('<IHHIIHH', 16, 1, 1, rate, rate * 2, 2, 16)
           + b'data' + struct.pack('<I', len(data)) + data)
    encoded = opus(wav)
    path = tmp_path / 'cue.ogg'
    path.write_bytes(encoded)
    native = json.loads(subprocess.check_output([source_tool, path], text=True))
    assert native == {'rate': rate, 'frames': frames}
    script = '''
import {readFileSync} from 'node:fs';
import assert from 'node:assert/strict';
import {AudioEngine, oggOpusInfo, decodeOggOpusSound} from './platforms/web/site/audio.js';
const bytes = new Uint8Array(readFileSync(process.argv[1]));
const info = oggOpusInfo(bytes);
let decodes = 0;
globalThis.EncodedAudioChunk = class {constructor(value) {Object.assign(this, value);}};
globalThis.AudioDecoder = class {
  constructor(callbacks) {this.callbacks = callbacks; decodes++;}
  configure(config) {this.channels = config.numberOfChannels;}
  decode(chunk) {assert.ok(chunk.data.length > 0);
    this.callbacks.output({numberOfFrames: 960, copyTo(plane) {plane.fill(0);}, close() {}});}
  async flush() {}
  close() {}
};
globalThis.AudioContext = class {
  destination = {};
  createGain() {return {connect() {}};}
  createBuffer(numberOfChannels, length, sampleRate) {
    return {numberOfChannels, length, duration: length / sampleRate, copyToChannel() {}};}
};
const audio = new AudioEngine(() => bytes, 2);
assert.equal(audio.duration('cue'), info.frames / info.rate);
assert.equal(audio.load('cue'), null);
const entry = await audio.whenLoaded('cue');
assert.equal(entry.rate, info.rate); assert.equal(entry.frames, info.frames);
assert.equal(audio.load('cue'), entry); assert.equal(decodes, 1);
assert.deepEqual(audio.errors, []);
if (info.frames) {
  const Decoder = globalThis.AudioDecoder;
  globalThis.AudioDecoder = class extends Decoder {decode() {}};
  await assert.rejects(() => decodeOggOpusSound(audio.context, bytes), /incomplete Opus decode/);
  globalThis.AudioDecoder = Decoder;
}
assert.throws(() => oggOpusInfo(bytes.subarray(0, bytes.length - 1)), /truncated/);
process.stdout.write(JSON.stringify(info));
'''
    browser = json.loads(subprocess.check_output(
        ['node', '--input-type=module', '-e', script, path], cwd=ROOT, text=True))
    assert browser['rate'] == rate and browser['frames'] == frames
    subprocess.run(['ffmpeg', '-v', 'error', '-xerror', '-i', path, '-f', 'null', '-'], check=True)
