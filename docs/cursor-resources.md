# Original Windows cursor resources

Löwenzahn 1's pinned `STARTW95.EXE` reports Director 5.0.1r26 and has SHA-256
`ee3d2075a1cc8a2c159f451e635d09a317d061489451f7b3bf304173f595caa0`.
Its initialization maps Lingo cursor 2 to the named `CrossCursor` resource
and cursor 3 to `PlusCursor` (calls at virtual addresses `0x45cdfe` and
`0x45ce35`). Those groups reference `RT_CURSOR` 3 and 4. The same routine
loads 0, 1 and 4 through `LoadCursorA(NULL, IDC_ARROW/IDC_IBEAM/IDC_WAIT)`;
those are operating-system roles rather than artwork stored in the projector.

The source policy pins the owning executable, each group binding and each
cursor payload. Conversion writes generated `D64C` assets into the image pack.
The compositor preserves the entire 32×32 canvas and hotspots `(8,8)` and
`(15,15)`. The cross consists of 33 screen-inversion pixels; the plus has black,
white and transparent pixels. Dropping the AND plane, cropping to 16 pixels,
or treating inversion as transparency changes the result. The resource layout
and AND/XOR behavior follow Microsoft's
[resource format documentation](https://learn.microsoft.com/en-us/windows/win32/menurc/resource-file-formats).

Clean ScummVM commit `41ac2b31847622d0662d22c03fe6979e3b43cfbc`,
`engines/director/cursor.cpp`, corroborates the numeric role names. Its Windows
custom-resource path is incomplete, so it does not establish this executable's
resource mapping. No ScummVM code is copied.

Synthetic Rust tests cover PE named/numeric resources, extent checks, group
bindings, source pins, duplicate IDs, row padding and all four mask outcomes.
Native tests cover destination inversion, hotspots, clipping, immutable cache
reuse and an actionable failure if a selected resource is absent.

The source-only native probe loads the unchanged executable as a data module,
then uses `LoadCursorA`, `GetIconInfo` and `DrawIconEx` over background RGB
`0x123456`. The actual Wine 8 API observations match both recovered resources
at all 1024 pixels with the original hotspots. This is original resource
evidence under Wine, distinct from an original-projector gameplay capture,
native Windows validation or physical Android qualification.

To repeat recovery after extracting the selected disc:

```sh
cargo run --manifest-path compiler/Cargo.toml --example executable_cursors -- \
  build/loewenzahn-1/sha256-f31970b980f0/extracted/MEDIA \
  games/loewenzahn-1/host/source-policy.json build/cursor-check
sh tools/reference/build-cursor-probe.sh
```

In a fresh private 32-bit Wine prefix, put the unchanged selected projector at
`drive_c/Original/STARTW95.EXE` and the generated probe at
`drive_c/cursor-probe.exe`. Run `wine 'C:\cursor-probe.exe'` under Xvfb, with
a bounded timeout. The probe writes `cursor-probe.txt`, `cursor-cross.bmp` and
`cursor-plus.bmp` into that private drive. It does not start or patch the game.
Verify the complete marker, loaded-resource success and all pixels with:

```sh
uv run --locked python tools/reference/compare_cursor_probe.py \
  build/cursor-check <private-prefix>/drive_c build/cursor-check/comparison.json
```

`D64C` is an internal generated asset, separate from the game package ABI:
magic `D64C`; big-endian u16 width, height, hotspot X, hotspot Y, version 1,
reserved zero; then width×height top-down bytes. Each byte keeps XOR in bit 0
and AND in bit 1: black 0, white 1, transparent 2, destination inversion 3.
Dimensions are bounded to 64×64. No original cursor pixels or generated
captures are committed.
