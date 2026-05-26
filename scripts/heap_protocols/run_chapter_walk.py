"""Full read protocol with HEAPDUMP + HEAPWALK at every checkpoint.

Drives the device through a realistic reading session: home baseline, file
browser, open the first book the BOOK_NAV sequence lands on, page through
PAGE_FORWARD_COUNT pages, open ReaderMenu, open ChapterSelection, back to
Reader, back to Home, settle. Captures the full block-level distribution at
every checkpoint into a JSON timeseries.

Usage:
  uv run python scripts/heap_protocols/run_chapter_walk.py LABEL

Pre-condition: SD card .crosspoint/ cleared if you want a clean parse trace;
otherwise the section comes from the cache and you'll only see deserialize
allocations rather than the full parse peak.
"""
import json
import sys
import time
from pathlib import Path

# Make `from devharness import Device` work regardless of where the repo is
# checked out — parent.parent is scripts/, which already has devharness/.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from devharness import Device  # noqa: E402

if len(sys.argv) < 2:
    print("usage: run_chapter_walk.py LABEL", file=sys.stderr)
    sys.exit(2)
label = sys.argv[1]
ts = time.strftime("%Y%m%d-%H%M%S")
out_path = Path(f"/tmp/heap_walk_{label}_{ts}.json")

# Tap sequence from FileBrowser root to the target book + CONFIRM.
# Tune for your SD card layout; default lands on the first book (UP wraps).
BOOK_NAV = [("UP", 1)]
PAGE_FORWARD_COUNT = 20
PAGE_BUTTON = "DOWN"

snapshots: list[dict] = []


def save() -> None:
    out_path.write_text(json.dumps({"label": label, "snapshots": snapshots}, indent=2))


def snap(d: Device, checkpoint: str) -> None:
    s = d.heap_dump()
    w = d.heap_walk()
    snapshots.append({
        "checkpoint": checkpoint,
        "ts": time.time(),
        "dump": {
            "free": s.free, "largest_free": s.largest_free, "min_free": s.min_free,
            "frag_pct": s.frag_pct, "alloc_blocks": s.alloc_blocks,
            "free_blocks": s.free_blocks, "total_blocks": s.total_blocks,
            "total": s.total,
        },
        "walk": {
            "used_buckets": w.used_buckets, "free_buckets": w.free_buckets,
            "top_used": w.top_used,
            "used_count": w.used_count, "used_bytes": w.used_bytes,
            "free_count": w.free_count, "free_bytes": w.free_bytes,
            "largest_used": w.largest_used, "largest_free": w.largest_free,
        },
    })
    save()
    top3 = " ".join(str(x) for x in w.top_used[:3])
    print(f"[{checkpoint:<22}] free={s.free:>6} largest={s.largest_free:>6} "
          f"used_count={w.used_count} top3={top3}", file=sys.stderr)


def main() -> int:
    print(f"output: {out_path}", file=sys.stderr)
    with Device() as d:
        d.go_home()
        time.sleep(2.0)
        snap(d, "home_baseline")

        d.go_home()
        d.navigate_home_to("FileBrowser")
        d.wait_for_activity("FileBrowser", timeout=60.0)
        time.sleep(1.0)
        snap(d, "filebrowser")

        for button, count in BOOK_NAV:
            for _ in range(count):
                d.tap(button)
                d.wait_for_refresh(timeout=60.0)
        d.tap("CONFIRM")
        d.wait_for_activity("EpubReader", timeout=60.0)
        time.sleep(2.0)
        snap(d, "book_opened")

        for i in range(PAGE_FORWARD_COUNT):
            d.tap(PAGE_BUTTON)
            d.wait_for_refresh(timeout=60.0)
            if (i + 1) % 5 == 0:
                snap(d, f"page_{i + 1}")

        # Push ReaderMenu, ChapterSelection.
        d.tap("CONFIRM")
        d.wait_for_activity("EpubReaderMenu", timeout=60.0)
        time.sleep(1.0)
        snap(d, "reader_menu")
        d.tap("CONFIRM")  # item 0 = SELECT_CHAPTER
        d.wait_for_activity("EpubReaderChapterSelection", timeout=60.0)
        time.sleep(1.0)
        snap(d, "chapter_selection")

        # BACK from ChapterSelection pops the modal back to Reader without
        # re-emitting [STATE] (Reader instance is resumed, not entered fresh).
        # Force CMD:STATE to confirm we're back on Reader.
        d.tap("BACK")
        time.sleep(1.5)
        cursor = d.lines_count()
        d.request_state()
        d.wait_for(lambda t: "[STATE] activity=EpubReader" in t, timeout=10.0, since=cursor)
        time.sleep(0.5)
        snap(d, "back_to_reader")

        d.go_home()
        time.sleep(2.0)
        snap(d, "home_after_t0")
        time.sleep(30.0)
        snap(d, "home_after_t30")

    print(f"\ndone. {out_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
