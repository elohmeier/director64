"""Explicit game selection and isolated source/build paths for checkout plugins."""

from __future__ import annotations

import importlib
import importlib.util
import os
import re
import sys
import tomllib
from dataclasses import dataclass
from pathlib import Path

SLUG = re.compile(r"[a-z0-9]+(?:-[a-z0-9]+)*\Z")


def work_dir() -> str:
    """Relative to the repository; also inherited by converter subprocesses."""
    value = os.environ.get("DIRECTOR64_WORK_DIR")
    if not value:
        raise ValueError("select a game with director64 <command> --game <slug>")
    return value


def repository() -> Path:
    return Path(__file__).resolve().parents[2]


@dataclass(frozen=True)
class GameSpec:
    root: Path
    slug: str
    data: dict
    source: dict

    @classmethod
    def load(cls, slug: str, source: str | None = None, root: Path | None = None):
        if not SLUG.fullmatch(slug):
            raise ValueError("invalid game slug")
        root = (root or repository()).resolve()
        data = tomllib.loads((root / "games" / slug / "game.toml").read_text())
        if data.get("schema_version") != 1 or data.get("slug") != slug:
            raise ValueError("invalid game manifest identity/schema")
        sources = data["sources"]
        ids = [s["id"] for s in sources]
        if len(ids) != len(set(ids)):
            raise ValueError("duplicate source identity")
        source = source or data["default_source"]
        selected = next((s for s in sources if s["id"] == source), None)
        if selected is None:
            raise ValueError(f"unknown source {source} for {slug}")
        digest = selected["sha256"]
        if not re.fullmatch(r"[0-9a-f]{64}", digest) or source != "sha256-" + digest[:12]:
            raise ValueError("source identity must match full SHA-256")
        images = [selected, *selected.get("alternates", [])]
        if selected.get("alternates") and selected["kind"] != "iso9660":
            raise ValueError("only disc image sources have alternate rips")
        for image in images:
            if Path(image["file"]).name != image["file"] or image["file"] in {".", ".."}:
                raise ValueError("source file must be a basename")
            if not re.fullmatch(r"[0-9a-f]{64}", image["sha256"]) or image["bytes"] <= 0:
                raise ValueError("source image needs a SHA-256 and a size")
        if len({i["file"].casefold() for i in images}) != len(images):
            raise ValueError("alternate rips need distinct file names")
        if len({i["sha256"] for i in images}) != len(images):
            raise ValueError("duplicate source image SHA-256")
        return cls(root, slug, data, selected)

    @property
    def directory(self) -> Path:
        return self.root / "games" / self.slug

    @property
    def work(self) -> Path:
        return self.root / "build" / self.slug / self.source["id"]

    @property
    def images(self) -> list[dict]:
        """The source's disc image and its alternate rips: other containers
        (a raw CloneCD `.img`, a `.bin`) of the same pressing, each pinned by
        its own SHA-256. The source id and `source["sha256"]` still name the
        edition."""
        return [self.source, *self.source.get("alternates", [])]

    @property
    def image(self) -> dict:
        """The catalogued image present under media/, else the primary one."""
        directory = self.root / "media" / self.slug / self.source["id"]
        return next((i for i in self.images if (directory / i["file"]).is_file()), self.source)

    @property
    def media(self) -> Path:
        return self.root / "media" / self.slug / self.source["id"] / self.image["file"]

    @property
    def dist(self) -> Path:
        return self.root / "dist" / self.slug / self.source["id"]

    def activate(self) -> None:
        os.environ.update(
            DIRECTOR64_GAME=self.slug,
            DIRECTOR64_SOURCE=self.source["id"],
            DIRECTOR64_WORK_DIR=self.work.relative_to(self.root).as_posix(),
        )
        package = self.data.get("port", {}).get("host_package")
        if package and package not in sys.modules:
            spec = importlib.util.spec_from_file_location(
                package,
                self.directory / "host/__init__.py",
                submodule_search_locations=[str(self.directory / "host")],
            )
            module = importlib.util.module_from_spec(spec)
            sys.modules[package] = module
            spec.loader.exec_module(module)

    def host(self, name: str):
        self.require_port()
        self.activate()
        return importlib.import_module(self.data["port"]["host_package"] + "." + name)

    def require_port(self):
        if self.data["status"] not in {"supported", "experimental"} or "port" not in self.data:
            raise ValueError(
                f"{self.slug} is {self.data['status']}; no executable port adapter exists yet"
            )


def selected_game() -> GameSpec:
    slug = os.environ.get("DIRECTOR64_GAME")
    if not slug:
        raise ValueError("no game selected")
    return GameSpec.load(slug, os.environ.get("DIRECTOR64_SOURCE"))
