# Autos bauen mit Willy Werkel

Experimental Director 6 port using the shared media engine and extended tables, of the German edition pinned in
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

The external car chooser uses the player's browser file dialog or Android
document picker. Imported cars pass the original ownership checks; exports
contain the original serialized car data. The certificate's original
PrintOMatic handler composes the car, names, signature and earned medals for
the platform print service. Documents use an A4 page with the authored margins.
Native transitions suspend the score during their reveal.

A substitute font supplies the name-width checks, and three text members lose
stray trailing characters so their literals parse. Full activity completion
and physical Android TV display qualification remain separate checks.
