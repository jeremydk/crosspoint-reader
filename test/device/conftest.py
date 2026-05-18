"""Pytest fixtures for on-device validation tests.

Tests run against a connected X3/X4 that's been flashed with an
ENABLE_SERIAL_LOG build (default `pio run` environment). One Device instance
is shared across the test session because opening the port can briefly
disturb the device on macOS, and re-opening per-test would also lose log
history that wait_for relies on.

Run from the repo root:
    uv run pytest test/device

Or against a specific port:
    DEVHARNESS_PORT=/dev/cu.usbmodem101 uv run pytest test/device
"""
from __future__ import annotations

import os
import pathlib
import sys

import pytest

# Make scripts/ importable without installing the package.
REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

from devharness import Device  # noqa: E402


@pytest.fixture(scope="session")
def device():
    port = os.environ.get("DEVHARNESS_PORT") or None
    with Device(port=port) as d:
        # Resync to a known state before the first test runs. wait_for_activity
        # will either see a fresh transition or the request_state echo.
        d.request_state()
        d.wait_for(lambda text: "[STATE]" in text, timeout=10.0)
        yield d


@pytest.fixture
def home(device):
    """Reset the device to Home before the test runs. Lets every test start
    from a known baseline regardless of what the previous test left behind,
    so tests compose as 'go home, then drive somewhere, then assert'."""
    device.go_home()
    return device
