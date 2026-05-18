"""CMD:HEAPDUMP smoke + activity-leak measurement.

The first batch verifies the heap-dump primitives round-trip against the
firmware. The activity-leak test exercises the higher-level workflow
(measure_activity_leak) you use to find activities that don't clean up
after themselves.
"""
from __future__ import annotations


def test_heap_dump_summary_has_sane_values(device):
    """Default CMD:HEAPDUMP returns a summary with positive free + largest
    and a fragmentation percentage in [0, 100]."""
    snap = device.heap_dump()
    assert snap.total > 0
    assert snap.free > 0
    assert snap.largest_free > 0
    assert snap.largest_free <= snap.free
    assert 0 <= snap.frag_pct <= 100
    assert snap.alloc_blocks > 0
    assert snap.total_blocks == snap.alloc_blocks + snap.free_blocks
    # Optional sections not requested, so they should be empty.
    assert snap.integrity_ok is None
    assert snap.by_cap == {}
    assert snap.regions == []


def test_heap_dump_with_caps_includes_internal_region(device):
    """CMD:HEAPDUMP CAPS gives per-capability breakdown. INTERNAL is always
    present on C3 (no PSRAM)."""
    snap = device.heap_dump(caps=True)
    assert "INTERNAL" in snap.by_cap
    assert "DEFAULT" in snap.by_cap
    internal = snap.by_cap["INTERNAL"]
    assert internal.free > 0
    assert internal.largest > 0


def test_heap_dump_with_regions_lists_per_internal_region(device):
    """CMD:HEAPDUMP REGIONS gives per-internal-region breakdown via
    heap_caps_print_heap_info(). The C3 has ~5 internal regions; we should
    parse at least two with positive total_blocks."""
    snap = device.heap_dump(regions=True)
    assert len(snap.regions) >= 2
    # At least one region should be substantially used (the main DRAM).
    busy = max(snap.regions, key=lambda r: r.total_blocks)
    assert busy.alloc_blocks > 10
    assert busy.largest_free > 0


def test_heap_dump_check_reports_ok_on_healthy_device(device):
    """A freshly booted device should pass integrity checks."""
    snap = device.heap_dump(check=True)
    assert snap.integrity_ok is True


def test_heap_snapshot_compare_renders_deltas(device):
    """Two snapshots taken back-to-back on an idle device should produce a
    diff that's mostly zeros (idle heap is stable)."""
    a = device.heap_dump()
    b = device.heap_dump()
    diff = b.compare(a)
    assert "free:" in diff
    assert "frag_pct:" in diff


def test_settings_round_trip_does_not_leak(home):
    """Enter Settings, back out, and verify alloc_blocks returns to
    baseline. Tolerance of 2 absorbs background allocations from the periodic
    MEM log line and similar."""
    def round_trip(d):
        d.navigate_home_to("Settings")
        d.wait_for_activity("Settings", timeout=10.0)
        d.tap("BACK")
        d.wait_for_activity("Home", timeout=10.0)

    report = home.measure_activity_leak(round_trip, label="Settings round-trip")
    assert report.looks_clean(block_tolerance=2), str(report)
