# Heap protocols

Autonomous data-collection scripts that drive a connected X3/X4 over USB CDC,
exercise a defined sequence of activities, and capture heap state at every
checkpoint. Output is JSON; one snapshot per checkpoint with the full
[HEAPDUMP] summary plus the [HEAPWALK] block-level distribution.

These complement the pytest-based on-device tests in `test/device/`. The
difference: those assert specific invariants per test; these collect a
*timeseries* of heap state during a realistic workload, intended for offline
analysis when chasing fragmentation, leaks, or peak-usage hot spots.

## Scripts

- **`capture_boot.py`** — sends `CMD:RESET` and captures the boot trace
  (including the five boot-phase HEAPWALK reports) into a log file. Use when
  investigating which boot phase owns a specific persistent allocation.
- **`run_chapter_walk.py`** — full read protocol with heap+walk at every
  checkpoint: home baseline → file browser → open book → page through 20 pages
  → reader menu → chapter selection → back → home. Use to measure heap
  evolution across a realistic reading session.
- **`explore_subactivities.py`** — focused on push/pop of sub-activities from
  Reader. Captures the per-stack-frame heap delta. Use to verify that adding
  a new sub-activity off Reader isn't introducing unexpected heap pressure.
- **`log_serial.py`** — generic serial tee. Reads /dev/cu.usbmodem* and writes
  every line to a file. Survives port hiccups via reopen. Use when running
  a long-form manual investigation and just need a record of the log stream.

## Prereqs

- Device flashed with `ENABLE_SERIAL_LOG` (default `pio run` env).
- Cache cleared (`.crosspoint/` on the SD card) for protocols that benefit
  from a clean state — most don't require it strictly, but mixing cached and
  cold data confuses interpretation.
- `pyserial` (`scripts/requirements.txt`).

## Run

```bash
uv run python scripts/heap_protocols/run_chapter_walk.py LABEL
uv run python scripts/heap_protocols/explore_subactivities.py LABEL
uv run python scripts/heap_protocols/capture_boot.py /tmp/boot.log
```

Output JSON path is printed on stdout. Each snapshot is saved incrementally
so an interrupted run still produces usable data.

## Reading the output

Each snapshot includes:
- `dump`: headline numbers from `CMD:HEAPDUMP` (free, largest_free, min_free,
  frag_pct, block counts).
- `walk`: distribution from `CMD:HEAPWALK` (used/free histograms by
  power-of-2 size bucket, top-10 largest used block sizes, summary counts).

The headline `frag_pct = 100 - (100 × largest_free / free)`. Above ~30% means
free RAM is split into chunks smaller than the next big contiguous allocation
needs (mbedTLS handshake wants ~50 KB contiguous).

Look at deltas between adjacent checkpoints to attribute allocation costs to
specific phases; look at `top_used` shape to identify single-block
fragmentation drivers; look at `used_buckets` distribution to distinguish
"many tiny allocations" (parser-state pattern) from "few large allocations"
(decode-workspace pattern).
