# Console cache-miss profile

The N64 is memory-bound: the VR4300's 8 KiB direct-mapped data cache and
16 KiB instruction cache against a megabyte of runtime state make a Lingo
step cost about 500 times what it costs on the host. This tool attributes
every cache fill of an emulator capture to the runtime function that caused
it and to the structure field it filled, so layout work is measured rather
than guessed.

## Emulator

The `cache-profile` branch of the local gopher64 checkout
(`~/repos/github.com/gopher64/gopher64`) counts instruction and data fills
per program counter and data fills per physical address whenever the
`GOPHER64_CACHE_PROFILE` environment variable names an output file
(`src/device/cache.rs`, module `cache_profile`). Build it with

```sh
OPENSSL_NO_VENDOR=1 cargo build --release
```

on that branch; it drops the renderer's thin-LTO flag because the host
clang is one LLVM major ahead of rustc. The histogram is rewritten every
four million data fills, so the last dump is within a few percent of the
end of the capture.

## Field table

`fields.c` encodes structure sizes and field offsets into symbol sizes;
compile it with the target compiler and read them back with nm:

```sh
docker run --rm -v "$PWD:/workdir" -w /workdir director64-toolchain:local sh -c \
  'mips64-elf-gcc -std=gnu17 -O2 -DDIRECTOR64_DIRECTOR_VERSION=8 -DDIRECTOR64_EXTENDED_D6=0 \
     -Iruntime/director -Iruntime/lingo -Iruntime/interaction -Iruntime/storage \
     -c tools/cache-profile/fields.c -o build/fields.o && mips64-elf-nm -S build/fields.o' \
  | python3 tools/cache-profile/fields.py > build/fields-d8.json
```

Regenerate it whenever `lv_runtime_t` or `dg_runtime_t` changes, or every
miss lands in the wrong field.

## Capture and attribute

```sh
GOPHER64=~/repos/github.com/gopher64/gopher64/target/release/gopher64 \
GOPHER64_CACHE_PROFILE=$PWD/build/profile.txt \
  uv run --locked director64 capture --game findus-mucklas train
docker run --rm -v "$PWD:/workdir" -w /workdir director64-toolchain:local \
  mips64-elf-nm -n -S /workdir/build/findus-mucklas/<source>/n64/probe/findus-mucklas-probe.elf \
  > build/symbols.txt
python3 tools/cache-profile/attribute.py build/profile.txt build/symbols.txt \
  build/findus-mucklas/<source>/activity-captures/<run>/train/gopher64.log build/fields-d8.json
```

The ROM prints `NATIVE_LAYOUT` at boot with both runtimes' addresses and,
since 2026-09-27, the start of the initialised data, the bss and the heap;
the report lists data misses by function, code misses by function and data
misses by structure field. Misses outside the two runtimes are the malloc
arena (overlays, images, buffers), the main binary's statics and the C
stack; the region of a hot bucket tells which.

## Misses by cache-period slice

The data cache is 8 KiB and direct-mapped, so two addresses 8 KiB apart
share one line and evict each other however far apart the code that
touches them is. The last table folds every 256-byte bucket into that
period, 32 slices, and names the objects in the four slices that miss
most: a field of either runtime, a static by its nm symbol (the first
object of the bucket; a bucket holds many small statics), a stack depth
below the top of RAM, an overlay or the heap. A slice whose misses are
split between two hot objects is a collision, and the fix is to move one
of them: the runtimes are placed by `director_main.c`, the static
sections and the heap start by `platforms/n64/n64.ld`.

## What it found on 2026-09-26

The first profile of the Mucklas train scene put a seventh of all data
misses in the mixer's per-pass volume calls, a sixth in audio-buffer
polling and timer reads, and half of all data accesses in idle passes of
the main loop that spun hundreds of times per tick; the fixes are in
`platforms/n64/director_main.c`. After them the misses that remain are the
interpreter's: the C stack (every 16-byte `lv_t` returned through memory
under the o64 ABI), the value stack and frames, the sprite table, the
property-list walks and the handler-table scans.

## Misses by caller

The profiler also buckets each data miss by the return address in effect
(`R` lines), which names the caller of a leaf helper (`memset`, `memmove`,
a string compare) that the PC alone attributes to libc. `attribute.py`
prints that table as "data misses by caller (return address)". The dump
runs at every million data misses, so a total is a floor to the nearest
million (the capture ends the emulator without a final dump).

## What it found on 2026-09-27

A B1 build of the same runtime measured 16% slower per Lingo step on the
train scene, with every phase (Lingo, render, audio) slower from the first
tick. The slice table put 22.7% of all data misses in one slice, shared by
the main stack's hottest frames and the cluster of small statics around
the .data/.bss boundary: the interrupt depth and status the exception
handler saves, the tick base every timer read adds, the display's frame
counters, and the runtime's cost counters. A 768-byte change to the code's
size had moved that cluster from a quiet part of the period onto the
stack's lines. `platforms/n64/n64.ld` now gives the read-only data, the
initialised data and the heap start fixed footings in the period, chosen
against this profile, and asserts at link time that the cluster stays off
the stack's slices.
