"""Composable navigation tests.

Each test starts from a known baseline (Home, via the `home` fixture which
calls device.go_home()) and drives the device to a specific activity, then
asserts on the transition. Tests are independent: any one can run by itself,
and they don't depend on order because each fixture run resets state.

navigate_home_to(symbol) reads the device's actual menu from a [HOME] log
line, so tests work whether OPDS is configured, recent books are present,
or the theme inlines Continue Reading. Symbols are stable across locales.

Available menu symbols:
    RecentBook<N>   one per recent book on disk
    FileBrowser
    Recents
    OpdsBrowser     only when OPDS servers are configured
    FileTransfer
    Settings
"""
from __future__ import annotations

import pytest


def test_navigate_to_file_browser(home):
    home.navigate_home_to("FileBrowser")
    home.wait_for_activity("FileBrowser", timeout=10.0)


def test_navigate_to_recents(home):
    home.navigate_home_to("Recents")
    home.wait_for_activity("RecentBooks", timeout=10.0)


def test_navigate_to_file_transfer(home):
    home.navigate_home_to("FileTransfer")
    home.wait_for_activity("CrossPointWebServer", timeout=10.0)


def test_navigate_to_settings(home):
    home.navigate_home_to("Settings")
    home.wait_for_activity("Settings", timeout=10.0)


def test_navigate_to_opds_when_configured(home):
    """Skips on devices without OPDS configured."""
    items, _ = home.home_menu()
    if "OpdsBrowser" not in items:
        pytest.skip("no OPDS servers configured on this device")
    home.navigate_home_to("OpdsBrowser")
    home.wait_for_activity("OpdsBookBrowser", timeout=10.0)


def test_back_from_settings_returns_to_home(home):
    """Round-trip: enter Settings, press Back, land on Home."""
    home.navigate_home_to("Settings")
    home.wait_for_activity("Settings", timeout=10.0)
    home.tap("BACK")
    home.wait_for_activity("Home", timeout=10.0)


def test_open_first_recent_book(home):
    """Skips on devices with no recent books."""
    items, _ = home.home_menu()
    if "RecentBook0" not in items:
        pytest.skip("no recent books on device")
    home.navigate_home_to("RecentBook0")
    home.wait_for_activity("Reader", timeout=15.0)
