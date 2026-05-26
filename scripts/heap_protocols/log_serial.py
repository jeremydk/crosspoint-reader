"""Generic serial tee: read /dev/cu.usbmodem* to a log file until interrupted.

Use when running a long-form manual investigation and you just need a record
of the log stream. Reopens on transient port errors so it survives the X4
flapping USB CDC under load.

One-shot lifecycle (no daemon mode, no PID file) so a stray instance can't
silently keep the serial port locked across a flash attempt. Ctrl-C to stop.

Usage:
  uv run python scripts/heap_protocols/log_serial.py /tmp/session.log
"""
import sys
import time

import serial

PORT = "/dev/cu.usbmodem2101"
out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/serial_logger.log"

ser = serial.Serial(PORT, 115200, timeout=0.3, dsrdtr=False, rtscts=False)
print(f"logging {PORT} -> {out} (Ctrl-C to stop)", file=sys.stderr)

with open(out, "w", buffering=1) as f:
    f.write(f"# logger started {time.strftime('%H:%M:%S')} port={PORT}\n")
    while True:
        try:
            data = ser.readline()
        except (serial.SerialException, OSError) as e:
            f.write(f"# read error: {e}\n")
            time.sleep(0.5)
            try:
                ser = serial.Serial(PORT, 115200, timeout=0.3, dsrdtr=False, rtscts=False)
                f.write(f"# reopened {time.strftime('%H:%M:%S')}\n")
            except Exception as e2:
                f.write(f"# reopen failed: {e2}\n")
            continue
        if not data:
            continue
        f.write(data.decode("utf-8", errors="replace"))
