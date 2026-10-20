# Fritz & Fertig

Experimental Director 8.5 port. The selected edition is pinned in
[game.toml](game.toml); [journeys.json](journeys.json) defines its regression route.

See [validation and remaining blockers](../../docs/additional-ports.md) before building.

```sh
uv run --locked director64 web --game fritz-fertig-1
uv run --locked director64 journey --game fritz-fertig-1
```
