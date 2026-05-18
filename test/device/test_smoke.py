"""Smoke tests for the serial control harness primitives.

These verify each primitive (request_state, tap, hold, wait_for_refresh)
round-trips against a live device. Navigation tests live in test_navigation.
"""
from __future__ import annotations

import pytest


def test_request_state_emits_fresh_state_line(device):
    """CMD:STATE should make the firmware re-emit [STATE] without requiring a
    transition. Wait for a [STATE] line emitted *after* the CMD so the test
    actually exercises the round-trip, not just whatever stale line already
    sits in the reader buffer from a previous transition."""
    device.request_state()
    # request_state's _write snapshots _send_cursor; wait_for defaults to
    # scanning from there, so the matching line must be in response to the CMD.
    line = device.wait_for(lambda text: "[STATE] activity=" in text, timeout=5.0)
    activity = line.split("activity=", 1)[1].strip()
    assert activity, "request_state produced an empty activity name"


def test_wait_for_refresh_resolves_after_tap(home):
    """A tap that changes the selector should produce a render-task
    notification within the refresh window."""
    home.tap("DOWN")
    home.wait_for_refresh(timeout=5.0)


@pytest.mark.parametrize("hold_ms", [50, 200, 500])
def test_hold_durations_round_trip(device, hold_ms):
    """Firmware should accept a range of HOLD durations and echo back the
    [HARNESS] confirmation line for each, catching parser regressions."""
    device.hold("DOWN", hold_ms)
    matching = device.wait_for(
        lambda text: f"hold {hold_ms}ms" in text,
        timeout=2.0,
    )
    assert f"btn DOWN hold {hold_ms}ms" in matching
