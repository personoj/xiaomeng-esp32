#!/usr/bin/env python3
"""Reproduce an offline before/after host playback microbenchmark.

Both binaries use the same current host fixtures. Production sources come from
--baseline-root and this checkout. No dependency installation or device/API use.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shlex
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-root", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path, default=ROOT / ".local/playback-benchmark")
    parser.add_argument("--repeats", type=int, default=5, help="alternating binary pairs; each does 9 adaptive CPU-time batches per size")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cjson-source", type=Path)
    parser.add_argument("--cjson-include", type=Path)
    parser.add_argument("--cjson-library")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    baseline = args.baseline_root.resolve()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    binaries = {}
    for label, root in (("before", baseline), ("after", ROOT)):
        build = out / ("build-" + label)
        command = [sys.executable, str(ROOT / "scripts/test_native.py"), "--source-root", str(root),
                   "--build-dir", str(build), "--benchmark", "--cc", args.cc]
        if label == "before":
            command.append("--legacy-ring-writes")
        for option in ("cjson_source", "cjson_include", "cjson_library"):
            if value := getattr(args, option):
                command += ["--" + option.replace("_", "-"), str(value)]
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / ("build-test-" + label + ".txt")).write_text(result.stdout, encoding="utf-8")
        if result.returncode:
            print(result.stdout, file=sys.stderr)
            result.check_returncode()
        binaries[label] = build / ("native_playback_bench.exe" if os.name == "nt" else "native_playback_bench")
    regression = {}
    for label, binary in binaries.items():
        test_binary = binary.with_name("native_playback.exe" if os.name == "nt" else "native_playback")
        result = subprocess.run([str(test_binary), "--counter-rollover-only"], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / ("counter-rollover-" + label + ".txt")).write_text(result.stdout, encoding="utf-8")
        regression[label] = {"returncode": result.returncode, "output": result.stdout}
    if regression["before"]["returncode"] == 0 or regression["after"]["returncode"] != 0:
        raise RuntimeError("Expected the published baseline to fail unread-cell preservation at rollover, and the candidate to pass.")
    raw = {"before": [], "after": []}
    # Alternate order to reduce monotonic warming/frequency drift bias.
    for pair in range(args.repeats):
        order = ("before", "after") if pair % 2 == 0 else ("after", "before")
        for label in order:
            result = json.loads(subprocess.check_output([str(binaries[label])], text=True))
            raw[label].append(result)
    assert len({r["checksum"] for runs in raw.values() for r in runs}) == 1
    timers = {r["clock"] for runs in raw.values() for r in runs}
    if len(timers) != 1:
        raise RuntimeError("Cannot compare benchmark runs using different CPU clocks.")
    summary = []
    for index in range(len(raw["before"][0]["runs"])):
        item = {"source_samples": raw["before"][0]["runs"][index]["source_samples"]}
        for label in raw:
            configs = [r["runs"][index] for r in raw[label]]
            times = [v for c in configs for v in c["cpu_ns_per_event"]]
            locks = {c["critical_entries_per_event"] for c in configs}
            assert len(locks) == 1
            item[label] = {"critical_entries_per_event": locks.pop(),
                           "batch_count": len(times), "cpu_ns_per_event_median": statistics.median(times),
                           "cpu_ns_per_event_min": min(times), "cpu_ns_per_event_max": max(times)}
        item["host_cpu_reduction_percent"] = 100 * (1 - item["after"]["cpu_ns_per_event_median"] /
                                                      item["before"]["cpu_ns_per_event_median"])
        summary.append(item)
    def digest(path: Path) -> str:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    report = {
        "generated_at_utc": datetime.now(timezone.utc).isoformat(),
        "scope": "Synthetic host CPU time for playback_event with preconstructed JSON, mocked decoder and instrumented host locks. No ESP32, real RTOS, API, acoustic, or end-to-end measurement.",
        "timing_unit": "Each sample is mean process CPU nanoseconds/event of an adaptive batch lasting at least 250ms CPU time; raw runs record each iteration count. Not a per-packet latency percentile.",
        "environment": {"platform": platform.platform(), "python": platform.python_version(),
                        "logical_cpus_reported": os.cpu_count(),
                        "cpu_timer": timers.pop(),
                        "compiler": subprocess.check_output(shlex.split(args.cc) + ["--version"], text=True).splitlines()[0]},
        "sha256": {"baseline_playback": digest(baseline / "main/playback.c"),
                   "candidate_playback": digest(ROOT / "main/playback.c"),
                   **{name: digest(ROOT / "tests" / name) for name in (
                       "native_playback_bench.c", "native_process_cpu.h", "native_mocks.h", "native_audio_helpers.h")}},
        "alternating_pairs": args.repeats, "summary": summary, "raw_runs": raw,
        "counter_rollover_regression": regression,
    }
    path = out / "playback-comparison.json"
    path.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    print("Evidence:", path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
