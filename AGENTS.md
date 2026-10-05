# Working in this repository

Commit to `main`. History is a linear series of direct commits; do not open a
branch for a change. If a change already sits on one, merge it back with
`git merge --ff-only` and delete the branch.

Never commit game media, files extracted or converted from it, ROMs, captures,
or links to them. Retail media stays under the ignored `media/`, generated
output under the ignored `build/` and `dist/`. Machine-specific settings go in
ignored `*.local.toml` files.

# Project references

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
projector comparison or N64 hardware validation.
