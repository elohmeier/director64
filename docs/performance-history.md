# Development and performance history

This is the chronological record that used to open the README: what each
port reached and what each optimization round measured and changed.

The port is being re-architected around its converter; the reasons, target
and milestones are in [roadmap.md](roadmap.md).

The planned browser player, local ISO import and GitHub Pages deployment are
described in [docs/web-roadmap.md](web-roadmap.md), including Rust conversion
work and game playability gates. `director64 web --game findus-workshop --serve` builds the
browser player locally: it imports Findus Workshop from your own ISO, converts
it in the browser and plays it ([docs/web.md](web.md)).

Director64 compiles recovered Director/Lingo programs into native Nintendo 64 ROMs.
Findus Workshop is the first supported game. Findus Mucklas is an experimental
Director 8 port. Löwenzahn 1 is an experimental Director 5 port with converted
video, native scripts, and a self-contained N64 ROM. Willy Werkel Cars is an
experimental Director 6 port with controller text entry and FlashRAM profiles.
Findus Weihnachten is an [experimental Director 7 calendar port](../games/findus-christmas/README.md).
Lernerfolg Deutsch 1-2 is an
[experimental Director 10 port](../games/lernerfolg-deutsch-1-2/README.md) with
playable exercises (Flash prompt cards render from their recorded edit
fields; the sanitized journey plays a spelling task end to end), an
emulator-qualified boot and a replay-driven showcase that types a login name
on the on-screen keyboard, visits a castle room and plays a spelling task
on the emulated console with faithful card states, umlauts and the original
school-font glyphs (the PFR decoder's curve semantics were corrected against
the disc's own Bitstream player); the ROM boots and plays on real hardware,
and closing the measured service-tick deficit is the next milestone. Every
game's ROM reports pacing windows (tick delivery, dropped service time,
audio positions, GC and live-object counts, and a per-phase render split)
that captures analyze through `director64.pacing`, and the perf sweep gained
a `--soak` stationarity mode ([details](fuzzing.md)). A first
optimization round memoized Lingo handler resolution, cutting host service
cost 23–27% and the Deutsch showcase's timing overruns from 1,348 to 198.
A second round fixed the image cache, whose headroom sweep had been evicting
each frame's own working set, and gave large images a faster codec: steady
scenes now load nothing at all (Deutsch's exercise screen reloaded ten images
every frame and now reloads none, dropping its render cost from 3.5 s to
0.9 s per five seconds of game time), and the showcase's dropped service time
fell from 15.6 s to 10.0 s with rendering no longer the dominant cost. A third
round measured scene entry — loading, not scripting — and cut the duplicate
overlay load each transition paid, the draw-order rebuild that strides every
channel, and the shared cast library each scene reloaded after the transition
between them released it, reaching 9.1 s. A fourth round found the remaining
cost in name dispatch itself: property reads and commands resolve by walking
chains of up to 222 string comparisons, which the size-optimized target emits
as real calls, so each link now rejects on its first character first. That
returned Löwenzahn's intro to full tick delivery — it had been running over
the service budget, which left its boot gate passing by a few percent — and
took the Deutsch showcase to 8.2 s. A fifth round moved to the work around
the interpreter. Löwenzahn's intro spins an authored wait loop on a linked
movie's rate, burning the whole 288-step budget every tick for a value that
changes once per tick; the compiler already yields such a loop at its back
edge, and teaching it about media and drag polls cut that scene's script
time from 3.29 s to 0.15 s per five seconds. The stage composite is now
skipped when nothing it draws has changed on every Director profile rather
than only Director 7 and later, which is what a static start screen was
paying 12 ms a frame for. Below Director 7 the collector ran every 256
allocations out of an 8,192-handle table, 47 to 96 ms of compaction each,
and now runs on the handles actually left — with its cost reported as
`gc_us`. Method resolution caches its lookups on every profile instead of
rescanning a movie's whole handler table, and a member search resolves the
requested name once per scan instead of once per member. Every game now
holds full 60 Hz delivery in its steady scenes; what remains is scene entry.
A sixth round measured one of those entries on the console itself and found
that nine tenths of it was linear searches of generated const tables, at
57 to 162 VR4300 cycles for every entry stepped over. Member name lookup
now binary-searches a generated folded-name index instead of striding
tens of kilobytes of member records; the question "does this member declare
a mouse handler", which hit testing asks of several sprites every tick, is
remembered per member instead of rescanning a movie's handler table; and a
property read confirms where that name was last found before walking the
key/value pairs again. A shared cast is also opened while the boot screens
are idle rather than inside the first scene entry that wants it. Service
work across Willy's boot fell 31% and Deutsch's 20%. A seventh round turned
to what those entries actually wait on, which is images: the console
decompresses maximum-compression assets at under a megabyte a second, so
three more ports now buy speed on anything a scene waits for through
`image_codec_tiers`. Findus Weihnachten holds full 60 Hz through its whole
boot, and Findus Workshop's and Willy's image work fell four to five times.
The three ports that could not follow were all blocked by the same thing,
which an eighth round found: a ROM may not exceed 56 MiB. The SDK's USB
debug channel reserves the last eight of the cartridge's 64 MiB as scratch
and writes every line the game prints to the start of it, so a larger ROM
has its own data overwritten while it runs — which is why Löwenzahn's video
decoder rejected a slice whenever a faster image codec grew the ROM past
that mark, and why the two ports already over it were quietly losing a few
hundred bytes. All six ROMs are now inside the ceiling, paid for by spending
a bit of each stored colour channel and a sample rate where they buy the
most: Löwenzahn 1.80 s of dropped service time to 0.11, Findus Mucklas 3.50
to 0.67. Two more local sources are catalogued. Every port now plays on all
four controllers: each console port carries a player with its own cursor,
drawn in its own colour, and Director's single mouse follows whichever of them
last pressed or pointed — a holder never loses it mid-click, so one controller
behaves exactly as it always did
([details](architecture.md#players-and-cursors)).
