"""The SDK builder accepts submodule gitfiles and rejects unpinned/dirty inputs."""

import json
import os
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest

from director64 import cli
from director64.toolchain import (
    PLATFORM_LABEL,
    RECIPE_LABEL,
    ToolchainError,
    inspect_image,
    recipe_fingerprint,
    record_build_context,
    selected_image,
)

ROOT = Path(__file__).resolve().parents[1]
BASE = "example.invalid/compiler@sha256:" + "a" * 64


def git(path, *args):
    return subprocess.check_output(["git", "-C", str(path), *args], text=True).strip()


@pytest.fixture
def checkout(tmp_path):
    repo = tmp_path / "game"
    repo.mkdir()
    git(repo, "init", "-q")
    sdk = repo / "third_party/libdragon"
    sdk.mkdir(parents=True)
    # A gitfile, like an initialized submodule or linked worktree, not a .git dir.
    git(sdk, "init", "-q", "--separate-git-dir", str(tmp_path / "sdk-git"))
    git(sdk, "config", "user.name", "Test")
    git(sdk, "config", "user.email", "test@example.invalid")
    (sdk / "fixture.txt").write_text("synthetic SDK input\n")
    git(sdk, "add", "fixture.txt")
    git(sdk, "commit", "-qm", "fixture")
    revision = git(sdk, "rev-parse", "HEAD")
    git(repo, "update-index", "--add", "--cacheinfo", f"160000,{revision},third_party/libdragon")
    (repo / "config").mkdir()
    (repo / "config/provenance.toml").write_text(
        f'[libdragon]\ncommit = "{revision}"\ncontainer_base = "{BASE}"\n'
    )
    (repo / "docker/toolchain").mkdir(parents=True)
    (repo / "docker/toolchain/Dockerfile").write_text(f"FROM {BASE}\n")
    (repo / ".mise-tasks").mkdir()
    (repo / "src/director64").mkdir(parents=True)
    shutil.copyfile(ROOT / "src/director64/toolchain.py", repo / "src/director64/toolchain.py")
    task = repo / ".mise-tasks/toolchain"
    shutil.copyfile(ROOT / ".mise-tasks/toolchain", task)
    bindir = tmp_path / "bin"
    bindir.mkdir()
    uv = bindir / "uv"
    uv.write_text(
        '#!/bin/bash\n'
        '[[ "$1" == run && "$2" == --locked ]] || exit 91\n'
        f'shift 5\nexec "{sys.executable}" "$@"\n'
    )
    uv.chmod(0o755)
    docker = bindir / "docker"
    docker.write_text(
        f"#!{sys.executable}\n"
        """import json, os, pathlib, sys
record = pathlib.Path(__file__).with_suffix('.json')
args = sys.argv[1:]
if args[0] == 'build':
    source = pathlib.Path(args[-1]) / 'libdragon-src'
    assert (source / 'fixture.txt').read_text() == 'synthetic SDK input\\n'
    assert not (source / '.git').exists()
    build_args = dict(args[i + 1].split('=', 1) for i, arg in enumerate(args)
                      if arg == '--build-arg')
    platform = args[args.index('--platform') + 1]
    assert platform == build_args['TOOLCHAIN_PLATFORM']
    assert len(build_args['TOOLCHAIN_RECIPE']) == 64
    image = {
        'Id': 'sha256:immutable', 'Os': platform.split('/')[0],
        'Architecture': platform.split('/')[1], 'context': args[-1],
        'Config': {'Labels': {
            'org.director64.libdragon-commit': build_args['LIBDRAGON_COMMIT'],
            'org.director64.toolchain-recipe': build_args['TOOLCHAIN_RECIPE'],
            'org.director64.toolchain-platform': platform,
        }},
    }
    record.write_text(json.dumps(image))
    if os.environ.get('MOCK_DOCKER_FAIL'):
        sys.exit(42)
    print('MOCK_DOCKER_BUILD_OK')
else:
    assert args[:2] == ['image', 'inspect']
    print(json.dumps([json.loads(record.read_text())]))
"""
    )
    docker.chmod(0o755)
    env = dict(os.environ, PATH=f"{bindir}:{os.environ['PATH']}")
    env.pop("LIBDRAGON_SOURCE", None)
    env["DOCKER"] = str(docker)
    env["DIRECTOR64_TOOLCHAIN_PLATFORM"] = "linux/amd64"
    return repo, sdk, task, env


def run_build(checkout):
    repo, _, task, env = checkout
    return subprocess.run(["bash", str(task)], cwd=repo, env=env, text=True, capture_output=True)


def test_toolchain_accepts_pinned_submodule_gitfile(checkout):
    assert (checkout[1] / ".git").is_file()
    result = run_build(checkout)
    assert result.returncode == 0, result.stderr
    assert "MOCK_DOCKER_BUILD_OK" in result.stdout


@pytest.mark.parametrize("problem", ["dirty", "wrong_revision", "uninitialized", "gitlink"])
def test_toolchain_rejects_invalid_source_before_docker(checkout, problem):
    repo, sdk, _, _ = checkout
    if problem == "dirty":
        (sdk / "fixture.txt").write_text("uncommitted change\n")
        expected = "checkout must be clean"
    elif problem == "wrong_revision":
        (repo / "config/provenance.toml").write_text('[libdragon]\ncommit = "wrong"\n')
        expected = "revision differs"
    elif problem == "uninitialized":
        (sdk / ".git").unlink()
        expected = "git submodule update --init --recursive"
    else:
        git(repo, "update-index", "--cacheinfo", f"160000,{'0' * 39}1,third_party/libdragon")
        expected = "revision differs"
    result = run_build(checkout)
    assert result.returncode != 0
    assert expected in result.stderr
    assert "MOCK_DOCKER_BUILD_OK" not in result.stdout


def test_toolchain_rejects_external_checkout_even_at_same_commit(checkout, tmp_path):
    _, sdk, _, env = checkout
    external = tmp_path / "external-sdk"
    subprocess.run(["git", "clone", "-q", str(sdk), str(external)], check=True)
    env["LIBDRAGON_SOURCE"] = str(external)
    result = run_build(checkout)
    assert result.returncode != 0
    assert "LIBDRAGON_SOURCE is unsupported" in result.stderr
    assert "MOCK_DOCKER_BUILD_OK" not in result.stdout


@pytest.mark.parametrize("revision", ["vendored", "old-sdk", None])
def test_container_must_match_vendored_revision(tmp_path, monkeypatch, revision):
    monkeypatch.setattr("director64.toolchain.source_revision", lambda root: "vendored")
    recipe = {"platform": "linux/amd64"}
    monkeypatch.setattr("director64.toolchain.image_recipe", lambda *args: recipe)
    image = {
        "Id": "sha256:immutable",
        "Os": "linux",
        "Architecture": "amd64",
        "Config": {
            "Labels": {
                "org.director64.libdragon-commit": revision,
                RECIPE_LABEL: recipe_fingerprint(recipe),
                PLATFORM_LABEL: "linux/amd64",
            }
        },
    }
    monkeypatch.setattr(
        "director64.toolchain.subprocess.check_output", lambda args: json.dumps([image]).encode()
    )
    if revision == "vendored":
        assert inspect_image(tmp_path, "mutable-tag")["Id"] == "sha256:immutable"
    else:
        with pytest.raises(ToolchainError, match="differs from vendored"):
            inspect_image(tmp_path, "mutable-tag")


@pytest.mark.parametrize(
    "changed",
    ["docker/toolchain/Dockerfile", ".mise-tasks/toolchain", "src/director64/toolchain.py"],
)
def test_existing_image_rejected_after_recipe_change(checkout, changed):
    repo, _, _, env = checkout
    result = run_build(checkout)
    assert result.returncode == 0, result.stderr
    path = repo / changed
    path.write_text(path.read_text() + "\n# changed build recipe\n")
    result = subprocess.run(
        [sys.executable, "-m", "director64.toolchain", "--root", str(repo)],
        env=env, text=True, capture_output=True,
    )
    assert result.returncode != 0
    assert "differs from build recipe" in result.stderr


@pytest.mark.parametrize("base", ["scratch", BASE.replace("a" * 64, "b" * 64)])
def test_builder_rejects_floating_or_unrecorded_compiler_base(checkout, base):
    repo, _, _, _ = checkout
    (repo / "docker/toolchain/Dockerfile").write_text(f"FROM {base}\n")
    result = run_build(checkout)
    assert result.returncode != 0
    assert "compiler base" in result.stderr
    assert "MOCK_DOCKER_BUILD_OK" not in result.stdout


@pytest.mark.parametrize("failure", [False, True])
def test_builder_cleans_staging_even_when_engine_fails(checkout, failure):
    _, _, _, env = checkout
    if failure:
        env["MOCK_DOCKER_FAIL"] = "1"
    result = run_build(checkout)
    assert (result.returncode != 0) == failure, result.stderr
    image = json.loads(Path(env["DOCKER"]).with_suffix(".json").read_text())
    assert not Path(image["context"]).exists()


def test_builder_rejects_nested_dependencies_instead_of_omitting_them(checkout):
    repo, sdk, _, _ = checkout
    revision = git(sdk, "rev-parse", "HEAD")
    (sdk / "nested").mkdir()
    git(sdk, "update-index", "--add", "--cacheinfo", f"160000,{revision},nested")
    git(sdk, "commit", "-qm", "nested dependency")
    revision = git(sdk, "rev-parse", "HEAD")
    git(repo, "update-index", "--cacheinfo", f"160000,{revision},third_party/libdragon")
    (repo / "config/provenance.toml").write_text(f'[libdragon]\ncommit = "{revision}"\n')
    result = run_build(checkout)
    assert result.returncode != 0
    assert "nested SDK submodules" in result.stderr
    assert "MOCK_DOCKER_BUILD_OK" not in result.stdout


@pytest.mark.parametrize("problem", ["recipe", "missing-recipe", "platform", "architecture"])
def test_inspector_rejects_stale_recipe_or_platform(checkout, problem):
    repo, _, _, env = checkout
    result = run_build(checkout)
    assert result.returncode == 0, result.stderr
    record = Path(env["DOCKER"]).with_suffix(".json")
    image = json.loads(record.read_text())
    if problem == "architecture":
        image["Architecture"] = "arm64"
    elif problem == "platform":
        image["Config"]["Labels"][PLATFORM_LABEL] = "linux/arm64"
    elif problem == "missing-recipe":
        del image["Config"]["Labels"][RECIPE_LABEL]
    else:
        image["Config"]["Labels"][RECIPE_LABEL] = "obsolete"
    record.write_text(json.dumps(image))
    result = subprocess.run(
        [sys.executable, "-m", "director64.toolchain", "--root", str(repo)],
        env=env, text=True, capture_output=True,
    )
    assert result.returncode != 0
    assert "run: mise run toolchain" in result.stderr


@pytest.mark.parametrize("previous", [None, "mutable-tag"])
def test_selected_image_pins_nested_operations_and_restores_environment(
    tmp_path, monkeypatch, previous
):
    if previous is None:
        monkeypatch.delenv("DIRECTOR64_TOOLCHAIN_IMAGE", raising=False)
    else:
        monkeypatch.setenv("DIRECTOR64_TOOLCHAIN_IMAGE", previous)
    monkeypatch.setattr(
        "director64.toolchain.inspect_image", lambda *args: {"Id": "sha256:immutable"}
    )
    with pytest.raises(RuntimeError), selected_image(tmp_path) as image:
        assert os.environ["DIRECTOR64_TOOLCHAIN_IMAGE"] == image["Id"] == "sha256:immutable"
        raise RuntimeError("failed conversion")
    assert os.environ.get("DIRECTOR64_TOOLCHAIN_IMAGE") == previous


@pytest.mark.parametrize(
    ("image", "arguments"),
    [("sdk-b", ["make", "PROFILE=release"]), ("sdk-a", ["make", "PROFILE=probe"])],
)
def test_build_context_invalidates_objects_only_when_inputs_change(tmp_path, image, arguments):
    record_build_context(tmp_path, "sdk-a", ["make", "PROFILE=release"])
    stamp = tmp_path / "n64/build-context.json"
    os.utime(stamp, ns=(1_000_000_000, 1_000_000_000))
    record_build_context(tmp_path, "sdk-a", ["make", "PROFILE=release"])
    assert stamp.stat().st_mtime_ns == 1_000_000_000
    record_build_context(tmp_path, image, arguments)
    assert stamp.stat().st_mtime_ns != 1_000_000_000
    assert json.loads(stamp.read_text()) == {"image": image, "make": arguments}


def test_build_refuses_assets_from_another_image_before_compilation(tmp_path, monkeypatch):
    game = SimpleNamespace(work=tmp_path, require_port=lambda: None)
    (tmp_path / "director").mkdir()
    (tmp_path / "director/packed.json").write_text('{"toolchain": {"image": "old-sdk"}}')
    monkeypatch.setattr("director64.build_inputs.verify", lambda game: None)
    monkeypatch.setattr("director64.rom_metadata.prepare", lambda *args: pytest.fail("metadata"))
    monkeypatch.setattr(cli, "run", lambda *args: pytest.fail("compilation"))
    with pytest.raises(ValueError, match="asset toolchain image changed"):
        cli._build(game, {"Id": "new-sdk"}, "release", 1)


def test_builder_uses_selected_platform(checkout):
    _, _, _, env = checkout
    env["DIRECTOR64_TOOLCHAIN_PLATFORM"] = "linux/arm64"
    result = run_build(checkout)
    assert result.returncode == 0, result.stderr
    image = json.loads(Path(env["DOCKER"]).with_suffix(".json").read_text())
    assert image["Architecture"] == "arm64"
    assert image["Config"]["Labels"][PLATFORM_LABEL] == "linux/arm64"
