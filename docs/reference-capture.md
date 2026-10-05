# Original projector reference

`director64 reference --game findus-workshop` captures the workshop's original
projector in an isolated Wine container using its selected source. Build the
container with `mise run reference-toolchain`. Optional `--replay`, `--output`,
`--source`, `--iso` and `--windows-version` arguments belong to this game's adapter.
Outputs stay under the selected source workspace's `reference/` directory.

The original-projector reference is independent of native and N64 evidence.
Its sandbox does not use normal user Wine or emulator save directories.
