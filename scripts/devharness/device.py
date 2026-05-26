"""Serial control + observation harness for a connected X3/X4.

Opens the device's USB CDC port once, runs a reader thread that captures every
log line, exposes a small API for sending CMD: messages and waiting on
[STATE] / log signals. Built to drive ENABLE_SERIAL_LOG firmware; release/slim
builds will silently ignore button-inject commands.

Caveat for macOS: opening /dev/cu.usbmodem* with default DTR pulses the C3 USB
peripheral and triggers a chip reset. We force DTR/RTS off before any traffic.
"""
from __future__ import annotations

import glob
import platform
import re
import threading
import time
from collections import deque
from dataclasses import dataclass, field

import serial


PHYSICAL_BUTTONS = {"BACK", "CONFIRM", "LEFT", "RIGHT", "UP", "DOWN", "POWER"}


@dataclass
class HeapCap:
    """Per-capability heap stats from CMD:HEAPDUMP CAPS."""
    free: int
    largest: int
    min_free: int
    alloc_blocks: int
    free_blocks: int


@dataclass
class HeapRegion:
    """One internal heap region from heap_caps_print_heap_info(). C3 splits
    DRAM into ~5 regions; the SDK only exposes block stats, not addresses
    or names. Order matches SDK output so you can compare by index across
    snapshots."""
    largest_free: int
    alloc_blocks: int
    free_blocks: int
    total_blocks: int


@dataclass
class HeapSnapshot:
    """Parsed CMD:HEAPDUMP response.

    frag_pct is the headline number: above ~30% means free RAM is split
    into chunks too small for the next big contiguous allocation (mbedTLS
    handshake, framebuffer clone) even when total free is comfortable."""

    total: int                          # total heap size (free + allocated)
    free: int                           # currently free bytes
    largest_free: int                   # largest single contiguous free block
    min_free: int                       # low watermark since boot
    alloc_blocks: int                   # count of allocated blocks
    free_blocks: int                    # count of free blocks
    total_blocks: int                   # alloc + free
    frag_pct: int                       # 100 - (100 * largest_free / free)
    by_cap: dict[str, HeapCap] = field(default_factory=dict)
    regions: list[HeapRegion] = field(default_factory=list)  # from REGIONS subcommand
    integrity_ok: bool | None = None    # None if CHECK wasn't requested

    def compare(self, other: "HeapSnapshot") -> str:
        """Human-readable diff: what changed from `other` to self."""
        def delta(a: int, b: int) -> str:
            d = a - b
            return f"{d:+d}" if d else "0"
        return (
            f"free:         {other.free} -> {self.free} ({delta(self.free, other.free)})\n"
            f"largest_free: {other.largest_free} -> {self.largest_free} "
            f"({delta(self.largest_free, other.largest_free)})\n"
            f"min_free:     {other.min_free} -> {self.min_free} ({delta(self.min_free, other.min_free)})\n"
            f"frag_pct:     {other.frag_pct}% -> {self.frag_pct}% "
            f"({delta(self.frag_pct, other.frag_pct)})\n"
            f"alloc_blocks: {other.alloc_blocks} -> {self.alloc_blocks} "
            f"({delta(self.alloc_blocks, other.alloc_blocks)})\n"
            f"free_blocks:  {other.free_blocks} -> {self.free_blocks} "
            f"({delta(self.free_blocks, other.free_blocks)})"
        )


@dataclass
class HeapWalk:
    """Parsed CMD:HEAPWALK response: full block-level snapshot of the default
    heap, bucketed by size. The headline question this answers is "what does
    the heap LOOK like right now" — many tiny holes vs few large holes, where
    the used bytes are concentrated, what single allocations are the
    fragmentation drivers."""
    # Bucket label -> count. Labels are upper bounds: bucket 32 covers (16, 32].
    used_buckets: dict[int, int]
    free_buckets: dict[int, int]
    top_used: list[int]            # 10 largest used block sizes, descending
    used_count: int
    used_bytes: int
    free_count: int
    free_bytes: int
    largest_used: int
    largest_free: int


@dataclass
class HeapLeakReport:
    """Delta across one enter+exit of an activity. alloc_blocks_delta != 0
    after exit means the activity (or a library it pulled in) didn't release
    every allocation. Pair with HeapSnapshot.compare for byte-level detail."""
    label: str
    before: HeapSnapshot
    after: HeapSnapshot

    @property
    def alloc_blocks_delta(self) -> int:
        return self.after.alloc_blocks - self.before.alloc_blocks

    @property
    def free_bytes_delta(self) -> int:
        return self.after.free - self.before.free

    @property
    def largest_free_delta(self) -> int:
        return self.after.largest_free - self.before.largest_free

    def looks_clean(self, block_tolerance: int = 0) -> bool:
        """True if alloc_blocks returned to baseline within tolerance.
        Tolerance absorbs noise from periodic background allocations
        unrelated to the activity under test."""
        return abs(self.alloc_blocks_delta) <= block_tolerance

    def __str__(self) -> str:
        verdict = "CLEAN" if self.looks_clean() else "LEAKED"
        return (
            f"{self.label}: {verdict} "
            f"(alloc_blocks {self.before.alloc_blocks} -> {self.after.alloc_blocks}, "
            f"free {self.before.free} -> {self.after.free} bytes, "
            f"largest_free {self.before.largest_free} -> {self.after.largest_free})"
        )


def _autodetect_port() -> str:
    system = platform.system()
    if system == "Darwin":
        # macOS exposes both /dev/cu.* and /dev/tty.* for the same USB CDC
        # device. Prefer /dev/cu.* (call-out): pyserial's DTR/RTS handling is
        # more reliable on cu.* than on tty.* under macOS.
        patterns = ["/dev/cu.usbmodem*", "/dev/tty.usbmodem*"]
    else:
        patterns = ["/dev/ttyACM*", "/dev/ttyUSB*"]
    for pat in patterns:
        candidates = sorted(glob.glob(pat))
        if candidates:
            return candidates[0]
    raise RuntimeError(
        f"no serial device found (searched {', '.join(repr(p) for p in patterns)}); "
        f"pass port=... explicitly"
    )


class Device:
    """One persistent serial connection that both observes logs and injects CMDs.

    Use as a context manager so the reader thread is torn down cleanly:

        with Device() as d:
            d.wait_for_activity("Home")
            d.tap("DOWN")
    """

    def __init__(self, port: str | None = None, baud: int = 115200) -> None:
        self.port = port or _autodetect_port()
        self.baud = baud
        self._ser: serial.Serial | None = None
        self._reader: threading.Thread | None = None
        self._stop = threading.Event()
        self._lines: deque[tuple[float, str]] = deque(maxlen=4096)
        self._lock = threading.Lock()
        self._cond = threading.Condition(self._lock)
        # Absolute, monotonic count of lines appended over the session's
        # lifetime. Diverges from len(self._lines) once the deque rotates
        # past maxlen. Cursors (_send_cursor, since= args to wait_for) are
        # also absolute, so they stay correct after rotation. _slice_since()
        # translates absolute → deque-local for actual scanning.
        self._lines_total: int = 0
        # Absolute line index just before the most recent CMD send. wait_for()
        # defaults to scanning from here so common patterns like
        # `hold(); wait_for(predicate)` aren't racy against the firmware
        # processing the CMD faster than we re-enter the Condition.
        self._send_cursor: int = 0

    def __enter__(self) -> "Device":
        # Construct without a port so DTR/RTS are configured BEFORE open()
        # instead of pulsing high and then being lowered after. macOS's USB
        # CDC driver still appears to perturb the C3 on first enumeration
        # regardless, so this doesn't fully prevent the chip reset, but it
        # eliminates the pyserial-side contribution. The wait_for_activity
        # in go_home() and the resync logic in conftest's session fixture
        # both tolerate the reset; tests run through it cleanly.
        self._ser = serial.Serial()
        self._ser.port = self.port
        self._ser.baudrate = self.baud
        self._ser.timeout = 0.1
        self._ser.dtr = False
        self._ser.rts = False
        self._ser.open()
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        return self

    def __exit__(self, *exc) -> None:
        self._stop.set()
        if self._reader is not None:
            self._reader.join(timeout=1.0)
        if self._ser is not None:
            self._ser.close()

    # ----- reader -----

    def _read_loop(self) -> None:
        assert self._ser is not None
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(256)
            except (OSError, serial.SerialException):
                time.sleep(0.1)
                continue
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").rstrip("\r")
                if not text:
                    continue
                with self._cond:
                    self._lines.append((time.time(), text))
                    self._lines_total += 1
                    self._cond.notify_all()

    # ----- inputs -----

    def _write(self, payload: str) -> None:
        """Send a CMD line. Snapshots the absolute line count first so the
        next wait_for() can scan lines emitted in response to this send, even
        if the firmware emits them before the host re-enters the Condition."""
        assert self._ser is not None
        with self._lock:
            self._send_cursor = self._lines_total
        self._ser.write(payload.encode("utf-8"))
        self._ser.flush()

    def press(self, button: str) -> None:
        button = button.upper()
        if button not in PHYSICAL_BUTTONS:
            raise ValueError(f"unknown physical button: {button}")
        self._write(f"CMD:BTN PRESS {button}\n")

    def release(self, button: str) -> None:
        button = button.upper()
        if button not in PHYSICAL_BUTTONS:
            raise ValueError(f"unknown physical button: {button}")
        self._write(f"CMD:BTN RELEASE {button}\n")

    def tap(self, button: str, hold_ms: int = 80) -> None:
        """Server-side tap: firmware schedules its own auto-release after
        hold_ms. Avoids round-trip latency on the release; the activity layer's
        wasReleased() edge still fires normally."""
        button = button.upper()
        if button not in PHYSICAL_BUTTONS:
            raise ValueError(f"unknown physical button: {button}")
        self._write(f"CMD:BTN TAP {button}\n" if hold_ms == 80
                    else f"CMD:BTN HOLD {button} {hold_ms}\n")

    def hold(self, button: str, ms: int) -> None:
        """Press and hold for `ms`, then auto-release. Same primitive as tap,
        named for readability when hold time is the point (e.g. power button
        long-press, settle windows)."""
        button = button.upper()
        if button not in PHYSICAL_BUTTONS:
            raise ValueError(f"unknown physical button: {button}")
        self._write(f"CMD:BTN HOLD {button} {ms}\n")

    def request_state(self) -> None:
        """Ask the firmware to re-emit the current [STATE] line. Useful after
        opening the connection to learn what activity we're on without waiting
        for the next transition."""
        self._write("CMD:STATE\n")

    def heap_dump(self, *, caps: bool = False, regions: bool = False,
                  check: bool = False, timeout: float = 5.0) -> HeapSnapshot:
        """Issue CMD:HEAPDUMP and parse the response. Summary line is always
        included. Flags add optional sections:
          caps     per-capability breakdown
          regions  per-internal-region (~5 on the C3)
          check    heap_caps_check_integrity_all()

        Per-block dump and heap_trace are not exposed; both require
        sdkconfig changes arduino-esp32 ships with disabled."""
        parts: list[str] = []
        if caps:
            parts.append("CAPS")
        if regions:
            parts.append("REGIONS")
        if check:
            parts.append("CHECK")
        if len(parts) >= 2:
            sub = " ALL"
        elif len(parts) == 1:
            sub = " " + parts[0]
        else:
            sub = ""
        self._write(f"CMD:HEAPDUMP{sub}\n")
        # Block until the firmware emits the end marker, so we know every
        # [HEAP] line for this dump is in the buffer before we parse.
        self.wait_for(lambda text: "[HEAP] end" in text, timeout=timeout)
        with self._lock:
            window = self._slice_since(self._send_cursor)
        return self._parse_heap_dump([text for _, text in window])

    def heap_walk(self, *, timeout: float = 10.0) -> HeapWalk:
        """Issue CMD:HEAPWALK and parse the block-level distribution.

        Use when CMD:HEAPDUMP's headline numbers (free, largest_free, frag_pct)
        aren't enough to pick the next intervention: HEAPWALK shows whether
        the heap is full of tiny allocs (PMR-arena candidate), a few medium
        ones (Tier-A static .bss candidate), or many small holes from
        subsystem residue (Tier-D heap-region candidate).

        The walk is O(blocks) on the device but blocks->no allocations during
        the walk itself (heap is locked). Cheap to call between checkpoints."""
        self._write("CMD:HEAPWALK\n")
        self.wait_for(lambda text: "[HEAPWALK] end" in text, timeout=timeout)
        with self._lock:
            window = self._slice_since(self._send_cursor)
        return self._parse_heap_walk([text for _, text in window])

    def measure_activity_leak(
        self,
        action,
        *,
        label: str = "",
        settle_s: float = 0.5,
    ) -> HeapLeakReport:
        """Run `action(self)` between two heap snapshots and report the delta.

        Typical use:
            d.go_home()
            report = d.measure_activity_leak(
                lambda d: (d.navigate_home_to("Settings"),
                           d.wait_for_activity("Settings"),
                           d.tap("BACK"),
                           d.wait_for_activity("Home")),
                label="Settings round-trip",
            )
            assert report.looks_clean(), str(report)

        settle_s gives deferred frees (render task, etc.) time to run
        before the post-snapshot. Bump it if false positives show up."""
        before = self.heap_dump()
        action(self)
        time.sleep(settle_s)
        after = self.heap_dump()
        return HeapLeakReport(label=label or "activity", before=before, after=after)

    _HEAP_SUMMARY_RE = re.compile(
        r"\[HEAP\]\s+total=(\d+)\s+free=(\d+)\s+largest=(\d+)\s+min_free=(\d+)\s+"
        r"alloc_blocks=(\d+)\s+free_blocks=(\d+)\s+total_blocks=(\d+)\s+frag_pct=(\d+)"
    )
    _HEAP_CAP_RE = re.compile(
        r"\[HEAP\]\s+cap=(\S+)\s+free=(\d+)\s+largest=(\d+)\s+min_free=(\d+)\s+"
        r"alloc_blocks=(\d+)\s+free_blocks=(\d+)"
    )
    _HEAP_CHECK_RE = re.compile(r"\[HEAP\]\s+check=(\S+)")
    # SDK's heap_caps_print_heap_info per-region line. Skips the final summary
    # line (which starts with "free " not "largest_free_block "); we already
    # have those numbers from the [HEAP] summary line we emit ourselves.
    _HEAP_REGION_RE = re.compile(
        r"largest_free_block\s+(\d+)\s+alloc_blocks\s+(\d+)\s+"
        r"free_blocks\s+(\d+)\s+total_blocks\s+(\d+)"
    )

    def _parse_heap_dump(self, lines: list[str]) -> HeapSnapshot:
        summary: HeapSnapshot | None = None
        caps: dict[str, HeapCap] = {}
        regions: list[HeapRegion] = []
        integrity_ok: bool | None = None
        in_regions_section = False
        for text in lines:
            if "[HEAP] regions_begin" in text:
                in_regions_section = True
                continue
            if "[HEAP] regions_end" in text:
                in_regions_section = False
                continue
            if in_regions_section:
                m = self._HEAP_REGION_RE.search(text)
                if m:
                    regions.append(HeapRegion(
                        largest_free=int(m.group(1)),
                        alloc_blocks=int(m.group(2)),
                        free_blocks=int(m.group(3)),
                        total_blocks=int(m.group(4)),
                    ))
                continue
            m = self._HEAP_SUMMARY_RE.search(text)
            if m:
                summary = HeapSnapshot(
                    total=int(m.group(1)),
                    free=int(m.group(2)),
                    largest_free=int(m.group(3)),
                    min_free=int(m.group(4)),
                    alloc_blocks=int(m.group(5)),
                    free_blocks=int(m.group(6)),
                    total_blocks=int(m.group(7)),
                    frag_pct=int(m.group(8)),
                )
                continue
            m = self._HEAP_CAP_RE.search(text)
            if m:
                caps[m.group(1)] = HeapCap(
                    free=int(m.group(2)),
                    largest=int(m.group(3)),
                    min_free=int(m.group(4)),
                    alloc_blocks=int(m.group(5)),
                    free_blocks=int(m.group(6)),
                )
                continue
            m = self._HEAP_CHECK_RE.search(text)
            if m:
                integrity_ok = (m.group(1) == "ok")
                continue
        if summary is None:
            raise RuntimeError(
                "CMD:HEAPDUMP response missing summary line; saw:\n  " + "\n  ".join(lines)
            )
        summary.by_cap = caps
        summary.regions = regions
        summary.integrity_ok = integrity_ok
        return summary

    _HEAPWALK_BUCKETS_RE = re.compile(r"\[HEAPWALK\]\s+(used|free)\s+(.+)$")
    _HEAPWALK_BUCKET_PAIR_RE = re.compile(r"b(\d+)=(\d+)")
    _HEAPWALK_TOP_RE = re.compile(r"\[HEAPWALK\]\s+top_used\s+(.+)$")
    _HEAPWALK_SUMMARY_RE = re.compile(
        r"\[HEAPWALK\]\s+summary\s+used_count=(\d+)\s+used_bytes=(\d+)\s+"
        r"free_count=(\d+)\s+free_bytes=(\d+)\s+largest_used=(\d+)\s+largest_free=(\d+)"
    )

    def _parse_heap_walk(self, lines: list[str]) -> HeapWalk:
        used_buckets: dict[int, int] = {}
        free_buckets: dict[int, int] = {}
        top_used: list[int] = []
        summary: tuple[int, int, int, int, int, int] | None = None
        for text in lines:
            m = self._HEAPWALK_BUCKETS_RE.search(text)
            if m:
                target = used_buckets if m.group(1) == "used" else free_buckets
                for pm in self._HEAPWALK_BUCKET_PAIR_RE.finditer(m.group(2)):
                    target[int(pm.group(1))] = int(pm.group(2))
                continue
            m = self._HEAPWALK_TOP_RE.search(text)
            if m:
                top_used = [int(x) for x in m.group(1).split()]
                continue
            m = self._HEAPWALK_SUMMARY_RE.search(text)
            if m:
                summary = tuple(int(m.group(i)) for i in range(1, 7))  # type: ignore[assignment]
                continue
        if summary is None:
            raise RuntimeError(
                "CMD:HEAPWALK response missing summary line; saw:\n  " + "\n  ".join(lines)
            )
        used_count, used_bytes, free_count, free_bytes, largest_used, largest_free = summary
        return HeapWalk(
            used_buckets=used_buckets,
            free_buckets=free_buckets,
            top_used=top_used,
            used_count=used_count,
            used_bytes=used_bytes,
            free_count=free_count,
            free_bytes=free_bytes,
            largest_used=largest_used,
            largest_free=largest_free,
        )

    def go_home(self, timeout: float = 10.0) -> None:
        """Test-harness escape: jump straight to Home regardless of current
        state. The firmware calls activityManager.goHome() so this works from
        any activity (Reader, FileBrowser, deep settings, etc.) without
        navigation guessing. Use at the start of each test for a clean
        baseline.

        Always sends CMD:HOME (no "already on Home" short-circuit) so the
        post-call window contains a fresh [STATE] activity=Home and a fresh
        [HOME] menu= line. Downstream callers like home_menu() rely on that
        freshness; skipping the send saved ~1s but broke them on second
        invocation from a Home baseline."""
        self._write("CMD:HOME\n")
        # _write snapshots _send_cursor; wait_for defaults to scanning from
        # that point so the [STATE] line emitted while we re-enter the
        # Condition is still seen.
        self.wait_for_activity("Home", timeout=timeout)

    def home_menu(self, timeout: float = 3.0) -> tuple[list[str], int]:
        """Return (items, selector) from the [HOME] menu= line emitted by the
        current Home session. Symbols are stable (FileBrowser, Recents,
        OpdsBrowser if configured, FileTransfer, Settings, plus RecentBook<i>
        entries for each recent book).

        Bounded to lines from after the most recent CMD (_send_cursor) so we
        don't pick up a stale selector value from a previous Home entry; the
        wait timeout covers the ~100ms gap between [STATE] activity=Home and
        Home's first render."""
        with self._lock:
            window = self._slice_since(self._send_cursor)
        for _ts, text in reversed(window):
            m = self._HOME_MENU_RE.search(text)
            if m:
                return m.group(1).split(","), int(m.group(2))
        # No [HOME] line emitted since the last CMD; wait for the next one.
        line = self.wait_for(
            lambda text: bool(self._HOME_MENU_RE.search(text)),
            timeout=timeout,
        )
        m = self._HOME_MENU_RE.search(line)
        assert m is not None
        return m.group(1).split(","), int(m.group(2))

    def navigate_home_to(self, target: str) -> None:
        """Tap Down/Up to reach the named menu item on Home, then Confirm.
        Reads the current menu from the [HOME] log so it adapts to the
        actual device config (OPDS configured, recent books present, etc.).
        Each step is bounded by wait_for_refresh's 3s timeout, so the worst
        case (full 7-item walk + Confirm) is under ~25s.

        Raises ValueError if target isn't a valid menu item on this device.
        """
        items, selector = self.home_menu()
        if target not in items:
            raise ValueError(
                f"{target!r} not in Home menu {items!r} on this device"
            )
        target_idx = items.index(target)
        delta = target_idx - selector
        button = "DOWN" if delta > 0 else "UP"
        for _ in range(abs(delta)):
            self.tap(button)
            self.wait_for_refresh(timeout=3.0)
        self.tap("CONFIRM")

    # ----- observation -----

    def lines_count(self) -> int:
        """Absolute count of lines captured over the session lifetime. Pair
        with wait_for's `since=` to wait for events produced after a specific
        moment (e.g. after sending a CMD). Survives deque rotation: returns a
        monotonically-increasing value even after older lines age out."""
        with self._lock:
            return self._lines_total

    def _slice_since(self, since: int) -> list[tuple[float, str]]:
        """Return _lines from absolute index `since` onwards. Caller must
        hold _lock. Translates the absolute index to a deque-local one,
        accounting for any lines that have aged out of the maxlen window."""
        base = self._lines_total - len(self._lines)
        local_start = max(0, since - base)
        return list(self._lines)[local_start:]

    def wait_for(self, predicate, timeout: float = 5.0, since: int | None = None) -> str:
        """Block until a captured log line satisfies predicate. Returns the
        matching line. Raises TimeoutError otherwise.

        `since` is an absolute index from lines_count(). If omitted, defaults
        to the snapshot taken just before the most recent CMD send, so common
        patterns like `hold(...); wait_for(...)` aren't racy. Pass
        since=lines_count() explicitly to scan only future lines, or since=0
        to scan everything still in the buffer."""
        deadline = time.time() + timeout
        with self._cond:
            start = since if since is not None else self._send_cursor
            while True:
                for _ts, text in self._slice_since(start):
                    if predicate(text):
                        return text
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError(f"wait_for timed out after {timeout:.1f}s")
                self._cond.wait(timeout=remaining)

    _STATE_RE = re.compile(r"\[STATE\]\s+activity=(\S*)")
    _REFRESH_RE = re.compile(r"\[REFRESH\]\s+done")
    _HOME_MENU_RE = re.compile(r"\[HOME\]\s+menu=(\S+)\s+selector=(\d+)")

    def wait_for_activity(self, name: str, timeout: float = 10.0, since: int | None = None) -> None:
        def matches(text: str) -> bool:
            m = self._STATE_RE.search(text)
            return bool(m and m.group(1) == name)
        self.wait_for(matches, timeout=timeout, since=since)

    def wait_for_refresh(self, timeout: float = 5.0) -> None:
        """Block until the next [REFRESH] done line emitted by the render task.
        Use after a tap that should produce a visible change, before asserting
        on the next state."""
        self.wait_for(lambda text: bool(self._REFRESH_RE.search(text)), timeout=timeout)

    def current_activity(self) -> str | None:
        """Most-recent [STATE] activity, if any. Returns None if no [STATE] has
        been seen yet; call request_state() to force one."""
        with self._lock:
            for _ts, text in reversed(self._lines):
                m = self._STATE_RE.search(text)
                if m:
                    return m.group(1) or None
        return None
