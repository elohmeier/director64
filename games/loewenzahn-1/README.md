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
chapter's recovered text and artwork on A4; Android TV shows a notice.

Known gaps:

- Cuts instead of authored transitions; OUTRO frame 10's palette change is
  not implemented.
- Executable-resource cursors and Cursor Asset Xtras are not implemented.
