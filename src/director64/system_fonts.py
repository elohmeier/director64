"""Pinned original system-font installers, read as data by the converter.

Keep redistributable installers intact and under their original filenames.
Extracted fonts only live in the user's generated game output.
"""

from __future__ import annotations

import hashlib
import shutil
import tempfile
import urllib.request
from pathlib import Path, PurePosixPath

INSTALLERS = {
    "85297a4d146e9c87ac6f74822734bdee5f4b2a722d7eaa584b7f2cbf76f478f6": {
        "file": "arial32.exe",
        "url": "https://downloads.sourceforge.net/project/corefonts/the%20fonts/final/arial32.exe",
    },
}
MAX_INSTALLER = 16 * 1024 * 1024


def installer(root: Path, digest: str) -> Path:
    """Fetch once, verify every use, and never execute the installer."""
    spec = INSTALLERS.get(digest)
    if spec is None:
        raise ValueError(f"no pinned system-font installer for SHA-256 {digest}")
    target = root / "build/system-fonts" / digest / spec["file"]
    if target.exists():
        if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
            raise ValueError(f"cached system-font installer hash mismatch: {target}")
        return target
    target.parent.mkdir(parents=True, exist_ok=True)
    request = urllib.request.Request(spec["url"], headers={"User-Agent": "Director64/1"})
    with urllib.request.urlopen(request, timeout=45) as response:
        data = response.read(MAX_INSTALLER + 1)
    if len(data) > MAX_INSTALLER or hashlib.sha256(data).hexdigest() != digest:
        raise ValueError(f"downloaded system-font installer hash mismatch: {spec['file']}")
    with tempfile.NamedTemporaryFile(dir=target.parent, delete=False) as handle:
        temporary = Path(handle.name)
        handle.write(data)
    try:
        temporary.replace(target)
    finally:
        temporary.unlink(missing_ok=True)
    return target


def relative_resource(path: str) -> PurePosixPath:
    relative = PurePosixPath(path)
    if (
        not path
        or relative.is_absolute()
        or ".." in relative.parts
        or "\\" in path
        or ":" in path
    ):
        raise ValueError(f"system-font resource must be media-relative: {path}")
    return relative


def prepare(root: Path, media: Path, policy: dict, *, disc: Path | None = None) -> None:
    """Place the policy's exact resources in its native media namespace."""
    for resource in policy.get("system_font_resources", []):
        path = resource["path"]
        relative = relative_resource(path)
        disc_path = resource.get("disc_path")
        source = relative_resource(disc_path) if disc_path is not None else None
        target = media / relative
        digest = resource["sha256"]
        if target.exists():
            if hashlib.sha256(target.read_bytes()).hexdigest() != digest:
                raise ValueError(f"system-font resource hash mismatch: {path}")
            continue
        if source is not None:
            if disc is None:
                raise ValueError(f"system-font disc extraction is missing: {disc_path}")
            original = disc / source
            if hashlib.sha256(original.read_bytes()).hexdigest() != digest:
                raise ValueError(f"system-font disc resource hash mismatch: {disc_path}")
        else:
            original = installer(root, digest)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
