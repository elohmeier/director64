# Sound channels

Sound channel methods and legacy commands have different time units. The
runtime implements `sound(n).fadeIn`, `fadeOut` and `fadeTo` over milliseconds,
with a default duration of 1000 ms. `fadeIn` starts at zero and returns to the
current volume; `fadeTo` can increase or decrease it. A replacement fade starts
at the current level, and an explicit volume write cancels it. Channel pan is
independent. The legacy `sound fadeIn` and `sound fadeOut` commands use ticks
(60 per second), with their tempo-derived default duration.

The service clock updates fades at 60 Hz. A positive duration shorter than a
service tick completes at the next tick. These are channel gains, separate
from the short mixer ramps used to avoid clicks when replacing a sound.

`sound(n).breakLoop()` disables repetition and continues from the current
sample through the sound's tail. It preserves pauses and does not change the
cast member's loop setting for future plays. Browser and Android audio retain
the request when a sound is still decoding. The browser service clock models
the same release position and remaining duration for sound waits and cue
queries. An audible host that cannot release a loop reports an error.

The method contracts are documented in Macromedia's
[MX Lingo Dictionary, fades (p.229)](https://www.manualshelf.com/manual/macromedia/director-mx-lingo-dictionary/user-guide-english/page-229.html)
and [breakLoop (p.108)](https://www.manualshelf.com/manual/macromedia/director-mx-lingo-dictionary/user-guide-english/page-108.html).
The distinction between legacy tick units and method milliseconds also appears
in the [sound fadeIn entry (p.611)](https://www.manualshelf.com/manual/macromedia/director-mx-lingo-dictionary/user-guide-english/page-611.html).

Source-free native tests cover interpolation, fade replacement, explicit
volume changes, legacy tick units and dispatch. Browser audio tests cover
loop release during playback, pause and asynchronous decoding, including the
release tail. These checks do not establish sample-exact behavior of the
original mixer or N64 hardware timing.

Compressed MP3 cast sounds are decoded in the shared Rust converter with
Symphonia 0.5.5, including browser imports. Existing decoded PCM takes
precedence when both representations are present. The decoder validates every
Layer III frame boundary before decoding; it accepts bounded ID3 metadata and
zero allocation padding, and rejects incomplete frames and format changes.
Recognized trailing metadata can begin in a final frame's ancillary area:
the complete audio frame remains intact, and only the tag's remaining bytes
are excluded from demuxing.

The recovered Advent sound that previously failed with an empty `snd `
resource contains 17 complete MP3 frames in `ediM`. Both this decoder and
strict FFmpeg decoding of those complete frames produce 19,584 mono samples
at 44,100 Hz. Their 16-bit PCM differs by at most one unit. The untrimmed
resource makes FFmpeg reject the metadata tail after decoding its audio;
this is why the explicit frame-boundary validation matters.

All 576 Optik MP3 cast allocations begin with one blank allocation frame.
The cast importer accepts that prefix only when its size matches the next
complete MP3 frame; ordinary MP3 and SWF streams retain strict framing.
Four original-projector controls report 1,332, 1,018, 1,227 and 417 ms,
matching decoded lengths of 58,752, 44,928, 54,144 and 18,432 frames at
44,100 Hz (the second is stereo). These checks used Director 8.5.1r104
under Wine 8; normalized MP3 decoding also agrees with strict FFmpeg to
within one PCM unit. They do not establish hardware audio timing.

Flash stream sound is decoded as one continuous PCM stream so MP3 reservoir
dependencies cross frame blocks. Authored seek offsets and initial latency
are retained. It plays on each Flash instance's dedicated media player,
separately from Director sound channels. Pause, seek, loop and child-movie
sound flags apply through that player. A missing stream block retains audio
from a preceding block, while leading empty frames remain silent.

The stream clock can skip displayed animation frames to preserve sound
synchronization; modeled Stop actions on skipped frames still execute. This
follows the streaming-sound contract in Adobe's
[SWF19 specification, p.181](https://open-flash.github.io/mirrors/swf-spec-19.pdf).
Director's Normal, Fixed and Lock-Step playback modes are separately modeled;
Direct-to-Stage can tick independently of the Director score. The runtime
tests cover pacing, instance overrides, stream gaps and controls. Original
8.5.1r104 probes establish the serialized mode/flag meanings; the combined
stream/mode behavior is not yet a sample-exact projector comparison.
