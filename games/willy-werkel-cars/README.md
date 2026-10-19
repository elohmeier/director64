# Autos bauen mit Willy Werkel

Experimental extended Director 6 port of the German edition pinned in
`game.toml` (a ZIP of the disc). The embedded launcher in `Willy32.exe`
converts as `START.DXR`.

```sh
uv run --locked director64 android build --game willy-werkel-cars
uv run --locked director64 web --game willy-werkel-cars --serve
uv run --locked director64 journey --game willy-werkel-cars
```

While driving, A accelerates, the pointer steers and B brakes or reverses;
notices are dismissed with A, B or Start (`runtime/willy_input.c`, switch
`WILLY`). Names are typed through the TV keyboard or the web app's text box.
The player and car database saves as `data.cxt`.

`game.toml` `[convert]`: the external file chooser becomes a recoverable
alert, the certificate's print button shows a notice, a substitute font is
measured for name-width checks, and three text members lose stray trailing
characters so their literals parse.

Known gaps: importing or exporting car files, printing, and the original's
score pause during transitions (the score keeps playing during reveals).
