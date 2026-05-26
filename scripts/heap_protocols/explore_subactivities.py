"""Drive into Reader, push sub-activities, back out; capture heap walks at
every transition. Goal: measure what each sub-activity costs in heap pressure
relative to its parent.

Diff strategy:
  reader_steady - home_baseline       = Reader's own footprint
  reader_menu_open - reader_steady    = ReaderMenu sub-activity's footprint
  chapter_sel_open - reader_menu_open = ChapterSelection's footprint
  reader_after_pop - reader_steady    = should be ~0 if no leak on unwind

Useful before adding a new sub-activity off EpubReader: run this against the
PR branch and compare to baseline. If the new activity adds more than a few
KB on top of reader_steady, that's worth investigating before merge.

Usage:
  uv run python scripts/heap_protocols/explore_subactivities.py LABEL
"""
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from devharness import Device  # noqa: E402

if len(sys.argv) < 2:
    print("usage: explore_subactivities.py LABEL", file=sys.stderr)
    sys.exit(2)
label = sys.argv[1]
ts = time.strftime("%Y%m%d-%H%M%S")
out_path = Path(f"/tmp/subact_{label}_{ts}.json")

BOOK_NAV = [("UP", 1)]
snapshots: list[dict] = []


def save() -> None:
    out_path.write_text(json.dumps({"label": label, "snapshots": snapshots}, indent=2))


def snap(d: Device, checkpoint: str) -> None:
    s = d.heap_dump()
    w = d.heap_walk()
    snapshots.append({
        "checkpoint": checkpoint,
        "ts": time.time(),
        "free": s.free, "largest_free": s.largest_free, "min_free": s.min_free,
        "frag_pct": s.frag_pct, "total": s.total,
        "used_buckets": w.used_buckets, "free_buckets": w.free_buckets,
        "top_used": w.top_used,
        "used_count": w.used_count, "used_bytes": w.used_bytes,
        "free_count": w.free_count, "free_bytes": w.free_bytes,
        "largest_used": w.largest_used,
    })
    save()
    print(f"[{checkpoint:<22}] free={s.free:>6} largest={s.largest_free:>6} "
          f"used_count={w.used_count} used_bytes={w.used_bytes} "
          f"top3={' '.join(str(x) for x in w.top_used[:3])}",
          file=sys.stderr)


def main() -> int:
    print(f"output: {out_path}", file=sys.stderr)
    with Device() as d:
        d.go_home()
        time.sleep(2.0)
        snap(d, "home_baseline")

        d.go_home()
        d.navigate_home_to("FileBrowser")
        d.wait_for_activity("FileBrowser", timeout=30.0)
        time.sleep(1.0)
        for button, count in BOOK_NAV:
            for _ in range(count):
                d.tap(button)
                d.wait_for_refresh(timeout=30.0)
        d.tap("CONFIRM")
        d.wait_for_activity("EpubReader", timeout=60.0)
        # Initial render + first-page parse.
        time.sleep(8.0)
        snap(d, "reader_first_page")

        # Page forward 5x so the section is hot and any per-page caches populated.
        for _ in range(5):
            d.tap("DOWN")
            d.wait_for_refresh(timeout=30.0)
        time.sleep(2.0)
        snap(d, "reader_steady")

        # Push ReaderMenu.
        d.tap("CONFIRM")
        d.wait_for_activity("EpubReaderMenu", timeout=20.0)
        time.sleep(2.0)
        snap(d, "reader_menu_open")

        # Push ChapterSelection (item 0 = SELECT_CHAPTER).
        d.tap("CONFIRM")
        d.wait_for_activity("EpubReaderChapterSelection", timeout=20.0)
        time.sleep(3.0)
        snap(d, "chapter_sel_open")

        # Pop ChapterSelection (no [STATE] is emitted on resume; force a query).
        d.tap("BACK")
        time.sleep(2.0)
        cursor = d.lines_count()
        d.request_state()
        d.wait_for(lambda t: "[STATE] activity=EpubReader" in t, timeout=10.0, since=cursor)
        time.sleep(1.0)
        snap(d, "reader_after_pop")

        d.go_home()
        time.sleep(3.0)
        snap(d, "home_after_t0")
        time.sleep(30.0)
        snap(d, "home_after_t30")

    print(f"\ndone. {out_path}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
