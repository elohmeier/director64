# Löwenzahn 1

Experimental Director 5 port of the German disc pinned in `game.toml` (an
alternate rip of the same pressing is accepted too). The game writes no
saves.

```sh
uv run --locked director64 android build --game loewenzahn-1
uv run --locked director64 web --game loewenzahn-1 --serve
uv run --locked director64 journey --game loewenzahn-1 [full_regressions|cursor_journey|printing_journey]
```

`[convert]` binds the 66 linked QuickTime movies and the external AIFF
sounds, substitutes Droid Sans for Geneva and Monaco, removes FIL frame 33's
30 fps tempo so the film activity's slider controls speed, and routes the
print stamp to `director64_print`. With switch `PRINTING`
(`runtime/printing.c`) the web app opens the browser's print dialog with the
chapter's recovered text and artwork on A4; Android uses its system print
dialog with that document content.

Authored transitions now run through the shared transition renderer.
OUTRO frame 10's palette change still needs a qualified original-projector
comparison.

Standard numeric Director cursor roles, monochrome bitmap/mask cursors and
the recovered single-frame 16-pixel Cursor Asset Xtra format are supported.
This disc's cursor 2 and 3 recover the original executable's cross and plus
resources, including full canvas, hotspots and screen inversion. The selected
engine uses Windows stock arrow, text-beam and wait cursors for 0, 1 and 4;
the port supplies those standard roles. Resource recovery matches all 1024
pixels of each original resource rendered by Windows APIs under Wine; that
comparison is distinct from native Windows or physical-device qualification.
