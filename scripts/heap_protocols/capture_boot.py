"""Trigger a clean software reset (CMD:RESET) and capture the boot trace.

ESP32-C3 native USB CDC doesn't honor host RTS as a reset signal — the line
wires to the CDC class, not the EN pin — so the only way to get a deterministic
fresh boot trace from the host side is to send CMD:RESET to running firmware.

The five boot-phase HEAPWALK reports emitted during setup() land in this log,
letting you attribute persistent allocations to specific init steps (settings
load, font setup, first paint, etc.).

Usage:
  uv run python scripts/heap_protocols/capture_boot.py /tmp/boot.log
"""
import sys
import time
from pathlib import Path

import serial

PORT = "/dev/cu.usbmodem2101"
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/boot_capture.log"

ser = serial.Serial(PORT, 115200, timeout=0.3, dsrdtr=False, rtscts=False)
# Drain anything buffered before our reset request.
ser.reset_input_buffer()
ser.write(b"CMD:RESET\n")
ser.flush()
print(f"reset issued; capturing to {out}", file=sys.stderr)

deadline = time.time() + 20.0
idle_since: float | None = None
with Path(out).open("w", buffering=1) as f:
    f.write(f"# boot capture started {time.strftime('%H:%M:%S')}\n")
    while time.time() < deadline:
        data = ser.readline()
        if data:
            idle_since = None
            f.write(data.decode("utf-8", errors="replace"))
        else:
            if idle_since is None:
                idle_since = time.time()
            elif time.time() - idle_since > 3.0:
                f.write("# capture done (idle 3s)\n")
                break
ser.close()
print("done", file=sys.stderr)
