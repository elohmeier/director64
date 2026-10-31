#!/bin/sh
# Freestanding 32-bit observation tool; no original engine bytes are linked.
set -eu
output=${1:-build/tools/cursor-probe}
case "$output" in build/*) ;; *) echo "output must be under build/" >&2; exit 2;; esac
mkdir -p "$output"
for library in kernel32 user32 gdi32; do
    llvm-dlltool -k -m i386 -d "tools/reference/cursor-probe-$library.def" \
        -l "$output/$library.lib"
done
clang --target=i686-pc-windows-msvc -std=c17 -Wall -Wextra -Werror \
    -ffreestanding -fno-builtin -c tools/reference/cursor-probe.c \
    -o "$output/cursor-probe.obj"
lld-link /entry:mainCRTStartup /subsystem:console /nodefaultlib /machine:x86 \
    "$output/cursor-probe.obj" "$output/kernel32.lib" "$output/user32.lib" \
    "$output/gdi32.lib" "/out:$output/cursor-probe.exe"
