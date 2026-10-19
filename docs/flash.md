# Flash playback

The shared runtime executes retained AVM1 actions and keeps a display list
for each Flash sprite. Reusable symbols have separate instances, playheads,
properties and placement generations. Removing a depth ends its display
lifetime while retained script references remain valid objects. Named raster
leaves, nested clips, buttons and edit fields participate in the display list.

Initializers execute once, in source order at their containing root frame,
before creating that frame's child objects. Registered classes supply each
instance's prototype and constructor. An action that exhausts its service
budget retains its stack, registers and call frames for the next service.
The scheduler completes pending work before advancing the same movie again.
Children keep their own playheads when the root is stopped. Director's
Normal, Fixed and Lock-Step clocks and stream sound are described in
[audio.md](audio.md).

The VM uses the existing value heap with external garbage-collection roots
for live stacks, lexical closures, prototypes and native service state. Its
limits are explicit: 512 operand slots, 32 call frames, 64 arguments per
call, 512 function definitions and 4,096 constants per pool. Each Flash root
also has limits of 2,048 instance records (including retired script-visible
objects until that root is disposed), 1,024 queued actions and 64 event-sound
voices. Each drawing object supports 8,192 points and 1,024 paths. Limit failures
include an AVM1 byte position. Conversion validates action records, nested
functions, branch destinations and operand formats, and rejects required
opcodes that the VM does not implement. An unavailable registered native
service remains a playback error with its function name.

The format contract comes from Adobe's
[SWF19 specification](https://open-flash.github.io/mirrors/swf-spec-19.pdf).
Versioned coercion follows the contemporary
[ActionScript 2 reference](https://open-flash.github.io/mirrors/as2-language-reference/constants.html).
In SWF5/6, undefined converts to numeric zero and an empty string; SWF7
converts it to NaN and `"undefined"`. Property names become case-sensitive
in SWF7. Windows-1252 action strings in older SWFs become UTF-8 values.
Director's external property and method interface resolves its case-insensitive
spelling to the authored ActionScript key.

An original-player experiment covers 24 observations across SWF5, SWF6 and
SWF7: undefined, null and primitive property writes; missing method calls;
undefined member reads; and undefined string/numeric conversions.
[generate_avm1_probe.py](../tools/reference/generate_avm1_probe.py) generates
the synthetic inputs and exposes both a `report` variable and debugger traces.
Writes through missing objects and calls to missing methods allow execution
to continue. Tests preserve that behavior, including in deployed component
constructors that refer to removed authoring placeholders.

Another 22 observations across SWF6/7 establish UTF-16 string indexing and
simple character case mapping: `Grüße😀!` has length eight and substring
`(2,5)` is `üße`; its uppercase form retains `ß`. Uppercasing the ligature
`ﬃ` leaves it unchanged, Greek `ΟΣ` lowercases to `οσ`, and `İ` lowercases
to `i`. Unicode full case expansions and contextual final-sigma rules do
not match these original-player observations.

A further 14 observations cover `super` across SWF6/7: inherited methods,
constructor calls, prototype data and Function2 preloading. In SWF6 an inherited
method's `super` lookup starts above the receiver's prototype; in SWF7 it starts
above the prototype that defines the method. Both preserve the actual receiver.
Function2 assigns parameter registers before preloads; valid local register
indices are below `registerCount`. All **60 original-player observations** are
replayed in the ordinary native test suite, alongside independent closure,
constructor, collection and interrupted-execution tests.

The experiment used Adobe's Windows standalone debugger **32,0,0,465** under
Wine 8 and Xvfb. Its SHA-256 is
`9d51df0c26e9ec4717a3da2e184cde831ca139d49e4a7c2f9e26028f8c4dfa52`;
the binary came from the
[official Adobe download](https://fpdownload.macromedia.com/pub/flashplayer/updaters/32/flashplayer_32_sa_debug.exe).
The player, generated SWFs and raw receipts remain ignored build artifacts.
This establishes original-player behavior under Wine for those inputs.
It does not establish native Windows or complete-game behavior.

Portable tests exercise the VM, native services, independent instances,
placement replacement, buttons, drawing and the Director/ActionScript bridge.
Recovered SoundControl initialization, timeline actions, live text and pointer
interaction run in an ignored sanitizer harness with the original Arial 2.82
font described below. Clicking its increment button changes the value from
zero to one; dragging the slider produces 153 in the field, component property
and Director sound-channel volume. The component's own callback performs the
update. MUSIKSTI button actions also execute in an ignored sanitizer harness.
These checks qualify those component paths, not every game interaction or the
audibility and timing of event sound on a playback device.

Flash masks retain separate geometric assets: fills contribute coverage
regardless of source alpha, clip alpha, visibility or color transforms, while
strokes contribute none. This applies through nested clips and dynamic drawing.
The source-free [mask probe](../tools/reference/generate_flash_mask_probe.py)
qualifies 16 cases against the same Adobe player: transparent bitmap fills and
embedded synthetic text contribute geometry, while device-font TextFields,
including fields with an opaque background, do not. An unmasked device-text
control verifies that the text itself renders. A child's own
`hitTest(x, y, true)` tests its shape without applying an ancestor's mask;
pointer routing applies the ancestor masks. Embedded live-text mask hits sample
the same glyph coverage as rendering, rather than the field rectangle. Missing
geometric assets or text raster/hit providers are explicit errors.

Gradient bevel conversion follows Adobe's SWF8 filter contract and is qualified
with nine [synthetic probes](../tools/reference/generate_filter_probe.py).
Zero-strength and unblurred endpoint cases are exact against Adobe Flash 32.
Across the measured image, the authored inner-blur parameters differ by at most
three per 8-bit color channel; doubling strength increases that maximum to five.
Outer and knockout effects differ by up to 15 in straight RGB at very low alpha,
with at most five in premultiplied color and three in alpha. Portable tests keep
sparse discriminating samples with individual bounds, usually exact or one,
and selected maximum-difference samples bounded from one to five. Full pixel
captures remain ignored. This is a qualified approximation of filter arithmetic,
not bit-identical output or a comparison with the selected Director Flash Xtra.

The compositor shared by the web and Android players draws live Flash
composition on the CPU into RGBA8888 surfaces. Allocation failures report the
requested image instead of omitting it. Portable tests cover overlapping
partial coverage, live Director film children and dynamic Flash drawing; they
do not establish game-scale speed or memory headroom. Birthday's opening
bedroom shows four blank bordered rectangles; they match the original
projector under Wine: they are authored save-name controls, and the recovered
script assigns empty names for the four empty save slots. The captures are at
different animation poses and do not establish whole-scene pixel parity.

Live Flash fields bind to an exact font family in recovered fonts or explicit
`system_font_resources`. A missing font is a conversion error with its movie,
cast and member. A resource records its media-relative path, SHA-256, exact
family aliases and, for an original installer, its CAB entry and extracted
font SHA-256. Conversions for the web player and the APK verify the same installer bytes;
the shared Rust converter extracts the bounded CAB entry without executing it.
The original SFNT bytes, metrics, character coverage and hinting are preserved.

The Deutsch edition needs Arial at 12 and 16 pixels. Its policy pins Microsoft's
original Arial 2.82 Core Fonts installer, `arial32.exe`. The redistributable
stays intact under its original filename; extracted fonts remain in the local
game build. Redistribution terms accompany the installer and are described in
[Microsoft's Core Fonts FAQ](https://corefonts.sourceforge.net/faq8.htm).
Six of SoundControl's eight embedded glyphs have the same contour and segment
topology as this font after contour rotation, with coordinates within 0.75 of
1,024 em units. Two glyphs have small quadratic segments simplified to lines.
This corroborates the font family; it does not prove the original installed
font revision or identical rasterization. The SWF itself omits layout metrics.
