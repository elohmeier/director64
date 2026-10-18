"""Module invocations must execute the CLI and propagate argument failures."""

import subprocess
import sys

import pytest


@pytest.mark.parametrize("module", ["director64", "director64.cli"])
def test_module_help(module):
    result = subprocess.run(
        [sys.executable, "-m", module, "--help"], capture_output=True, text=True,
    )
    assert result.returncode == 0
    assert "usage:" in result.stdout
    assert "doctor" in result.stdout


@pytest.mark.parametrize("module", ["director64", "director64.cli"])
def test_module_rejects_unknown_command(module):
    result = subprocess.run(
        [sys.executable, "-m", module, "not-a-command"], capture_output=True, text=True,
    )
    assert result.returncode == 2
    assert "invalid choice" in result.stderr
