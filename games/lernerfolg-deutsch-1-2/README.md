# Lernerfolg Grundschule Deutsch 1-2

Experimental Director 10 (MX 2004) port of the German disc pinned in
`game.toml` (800×600 stage). The full chain plays: launcher, controller
window, intro, login with name entry, class pick, the castle map, its rooms
and the exercises.

```sh
uv run --locked director64 android build --game lernerfolg-deutsch-1-2
uv run --locked director64 web --game lernerfolg-deutsch-1-2 --serve
uv run --locked director64 journey --game lernerfolg-deutsch-1-2
```

`[convert]` streams the `voc` and `sfx` folders, ships the `tests_db`
exercise databases as data, measures every authored font size for text
layout, and replaces the two JavaScript-dialect handlers (`int2hex`,
`clearGarbage`) with runtime natives. `[player]` lists the preference and
progress files and the per-player save files, and fixes `the long date`.

Known gaps:

- The controller window's own score and its visible statistics panel are
  not rendered.
- `TASKSCRIPTS.CXT initFields` in `TEST2CB1.DXR` asks for metrics of a font
  the converter does not record.
