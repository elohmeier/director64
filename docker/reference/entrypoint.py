#!/usr/bin/env python3
"""Bound decoder workers before starting the isolated 32-bit Wine process."""

import json
import os
import sys
from pathlib import Path

# GLib/libav size their worker pools from CPU affinity. On large hosts those
# stacks can exhaust an old 32-bit projector's address space before host RAM.
available = sorted(os.sched_getaffinity(0))
selected = available[:4]
os.sched_setaffinity(0, selected)
Path("/session/host-resources.json").write_text(
    json.dumps({"available_cpus": available, "cpu_affinity": selected,
                "llvmpipe_workers": os.environ.get("LP_NUM_THREADS")}, indent=2) + "\n"
)
os.execv(sys.executable, [sys.executable, "/runner/reference_runner.py", "_capture", *sys.argv[1:]])
