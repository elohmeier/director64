# Working in this repository

Commit to `main`. History is a linear series of direct commits; do not open a
branch for a change. If a change already sits on one, merge it back with
`git merge --ff-only` and delete the branch.

Never commit game media, files extracted or converted from it, APKs, web builds,
captures or links to them. Retail media stays under the ignored `media/`, generated
output under the ignored `build/` and `dist/`. Machine-specific settings go in
ignored `*.local.toml` files.

# Project references

Implement the behavior of the selected Director version, including features
missing from other players. Use original-projector experiments, contemporary
Macromedia/Adobe documentation, and recovered game requirements to resolve
ambiguities. Keep synthetic experiments reproducible and record the projector
version, input, expected output, and execution environment. Wine results are
original-projector evidence under Wine, not native Windows validation.
Check loaded runtime DLLs as well as the launcher: a thin projector executable
can advertise a different Director version from its actual playback engine.

For Director/Lingo behavior and recovered score formats, use a local checkout
of [ScummVM](https://github.com/scummvm/scummvm) as a behavioral reference.
Check its revision and worktree before relying on it, and cite the commit.
ScummVM is GPL: consult it for format facts and behavior, never copy its code.

Relevant code is under `engines/director/`: `frame.cpp` decodes versioned score
records, `score.cpp` handles tempo and frame events, and `lingo/` implements
language services. Select behavior for the version declared by the selected game
and verified in its media. The first supported game uses Director 6; other
versions have different tempo encodings and event semantics.

Corroborate reference behavior with the recovered local game data and focused
tests. ScummVM implementation agreement is distinct from a successful original
projector comparison or validation on the Android TV.

Capability failures protect builds while implementation is incomplete; they do
not count as implementing the rejected feature. Replace silent success with
actionable diagnostics, then implement and test the required behavior.
