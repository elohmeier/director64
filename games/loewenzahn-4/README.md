# Löwenzahn 4

Experimental Director 7 port. The selected edition is pinned in
[game.toml](game.toml); [journeys.json](journeys.json) defines its regression route.

See [validation and remaining blockers](../../docs/additional-ports.md) before building.

```sh
uv run --locked director64 web --game loewenzahn-4
uv run --locked director64 journey --game loewenzahn-4
```

The original player can also run in the isolated Wine reference environment:

```sh
uv run --locked director64 reference --game loewenzahn-4 --windows-version win95
```

The reference recipe bypasses the CD audio protection check in a hash-verified
private copy of `OPTGRAPH.DLL`; the source ISO and playback engine are untouched.
Captures explicitly record this modification. Add `--unmodified` to reproduce
the original check. See [the compatibility details](../../docs/validation.md).
