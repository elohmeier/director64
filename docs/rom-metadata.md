# ROM metadata

Each supported game's `metadata/metadata.ini` supplies its menu description,
port credits, release date, website, and number of players. Workshop's file is
[`games/findus-workshop/metadata/metadata.ini`](../games/findus-workshop/metadata/metadata.ini).
The date is an explicit release date, not the current time on each rebuild.
The port credits identify the N64 adaptation. Director64's code is MIT-licensed,
but a built ROM contains assets converted from the user's disc and carries no
license to redistribute them.

`director64 build --game findus-workshop` generates
`build/<game>/<source>/n64/<profile>/metadata.ini`. It prefixes `short-desc` with
the package version from `pyproject.toml` and the first 12 characters of Git HEAD.
Uncommitted or untracked changes add `-dirty`; diagnostic builds add `-probe`.
Comments record the full revision, selected source ID and SHA-256, and profile.
The internal header title stays stable across builds.

Generation preserves the file's timestamp when its contents have not changed.
The generated INI is a prerequisite of the ROM. Changing metadata
and rerunning the build therefore updates the ROM. `--reuse-assets` can be used
when the existing source/compiler/ABI receipt is still valid.

The pinned SDK's `n64.mk` emits `--padding 0` when metadata is configured before
inclusion, but its `n64tool` requires a suffix for one-digit sizes. The project
works around this by setting `N64_ROM_METADATA` after the include and using
`--padding 0B` to disable padding for the initial ROM.
This keeps the SDK pin unchanged and leaves final alignment to `n64metadata`.

The pinned container's `n64metadata` appends the ZIP, sets its header flag, and
pads the final ROM to 16 KiB. Validation and SHA-256 calculation happen afterward.
Build validation compares the embedded INI against the generated input. Both
build validation and SummerCart copying check ZIP CRCs, unique entry names,
bounded extraction, valid UTF-8, required menu fields, and text length limits.
`short-desc` is limited to 120 UTF-8 bytes; other values to 255 bytes.

Header validation requires:

| Bytes | Meaning |
| --- | --- |
| `0x34–0x37` | `00` per declared port, `FF` for the rest; `00 00 00 00` for the four-player ports |
| `0x38 = 01` | Embedded metadata ZIP present |
| `0x3C–0x3D = ED` | Advanced homebrew header |
| `0x3F = 52` for Workshop | FlashRAM, region-free, RTC disabled |

`num-players` in the game's `metadata/metadata.ini` must equal the
`controller_count` its source policy declares, which is also the number of ports
the runtime reads and the number of cursors it can draw. Older ROMs without
these declarations need rebuilding before the copy command will accept them. The menu's legacy “Version” field displays the raw `0x3F` byte
(82 for Workshop), so application version information belongs in the description.

N64FlashcartMenu added embedded text in V0.3.2 and player count in V0.3.3.
This is a menu feature, separate from SummerCart firmware. The inspected
[V0.3.3 parser](https://github.com/Polprzewodnikowy/N64FlashcartMenu/blob/8c5fb11fa5f3bc4f26670e1bb03a25557f725a6a/src/menu/rom_info.c)
prefers external metadata with a nonempty `name` over embedded text. Existing
`findus-workshop.meta` ZIP or `findus-workshop.metadata.ini` sidecars can therefore
override the new description; the copy command preserves them.

The inspected menu loads cover art from SD files, not the embedded ZIP.
`summercart --artwork cover.png` validates PNG chunk CRCs and dimensions, then
copies it to `menu/metadata/homebrew/Findus Workshop/boxart_front.png` on the same
card as the ROM. The supplied file must be 158×112 or 112×158 pixels and no larger
than 1 MiB. Symlink destinations are rejected. Copies are verified before file
replacement; saves and other images are preserved, and the card is unmounted
afterward. `--dry-run` checks inputs and prints destinations without mounting.

Container validation establishes packaging integrity. It does not establish
display correctness on the installed menu or successful N64/M64 hardware boot.
