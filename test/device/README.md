# On-device validation tests

Pytest tests that drive a connected X3/X4 over USB CDC.

## Prereqs

- Device flashed with an `ENABLE_SERIAL_LOG` build (default `pio run` env).
- `pyserial` + `pytest` (already in `scripts/requirements.txt`).
- USB cable connected to a `/dev/cu.usbmodem*` (macOS) or `/dev/ttyACM*` (Linux).

## Run

```bash
uv run pytest test/device                       # autodetect port
DEVHARNESS_PORT=/dev/cu.usbmodem101 uv run pytest test/device
uv run pytest test/device/test_smoke.py -v      # one file
```

## How it works

The harness opens the serial port once (`scripts/devharness/Device`) and runs
a reader thread that captures every log line into a deque. CMDs are sent over
the same connection. macOS DTR/RTS are forced off on open because the default
pulse triggers `USB_UART_CHIP_RESET` on the C3.

## Adding tests

Use the `device` fixture (session-scoped) or `home` fixture (skips if not on
Home). Pattern:

```python
def test_open_book(home):
    home.navigate_home_to("RecentBook0")
    home.wait_for_activity("Reader", timeout=15.0)
    home.wait_for_refresh()
    home.tap("BACK")
    home.wait_for_activity("Home")
```

See `scripts/devharness/device.py` for the full Device API.
