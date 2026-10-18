#!/usr/bin/env bash
# Run the opt-in diagnostics; a nonzero exit means known gaps remain.
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p build/audit
status=0
for profile in d8 d10 d85; do
    options=(-DDIRECTOR64_NATIVE_STAGE=1)
    case "$profile" in
        d8) options+=(-DDIRECTOR64_DIRECTOR_VERSION=8) ;;
        d10) options+=(-DDIRECTOR64_DIRECTOR_VERSION=10 -DDIRECTOR64_EXTENDED_D6=1) ;;
        d85) options+=(-DDIRECTOR64_DIRECTOR_VERSION=10 -DDIRECTOR64_EXTENDED_D6=1 -DDIRECTOR64_D85=1) ;;
    esac
    "${CC:-cc}" -std=c17 -Wall -Wextra -Werror -pedantic "${options[@]}" \
        -Iruntime/director -Iruntime/lingo -Iruntime/interaction -Iruntime/storage \
        -Iplatforms/web tools/audit/visual_probe.c platforms/web/compositor.c \
        runtime/director/director.c runtime/director/shape.c runtime/lingo/lingo_runtime.c \
        -lm -o "build/audit/visual-$profile"
    echo "$profile"
    "build/audit/visual-$profile" || status=1
done
exit "$status"
