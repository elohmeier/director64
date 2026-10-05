# Independent conformance catalog

These definitions inventory source obligations and independent conformance
assertions. Their evidence statuses are separate from the completed game port’s
functional release receipt. No historical prototype captures qualify current code.

```sh
uv run --locked director64 compatibility --game findus-workshop validate --inventory-mode optional
uv run --locked director64 compatibility --game findus-workshop report --format markdown
```

The current game qualification is produced by `director64 release --game
findus-workshop` from the game’s sanitized native journeys and matching N64 captures.
Local conformance overlays live in the selected source workspace; they are ignored
and must pin matching artifacts. `DIRECTOR64_LOCAL_EVIDENCE` or `--local-evidence`
selects an explicit overlay. The shared validation machinery remains source-free.
