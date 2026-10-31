# Lernerfolg Grundschule Deutsch 1-2

Experimental Director 10 (MX 2004) port of the German disc pinned in
`game.toml` (800×600 stage). The full chain plays: launcher, controller
window, intro, login with name entry, class pick, the castle map, its rooms
and the exercises.

The controller keeps an independent score and clock while hidden. Its authored
Control+Shift+Q shortcut opens the statistics panel over the running game; the
registered journey exercises that path after selecting an exercise answer.
It then uses the panel's Sequence Info controls to open `TEST2CB1.DXR` and
checks completed answer ranges, text layout, editable fields and their live
vector backgrounds.

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
