#!/usr/bin/env python3
"""Read-only, bounded wake diagnostics. Never records PCM, transcripts or keys."""
from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import math
from pathlib import Path
import time

ROOT = Path(__file__).resolve().parents[1]
STATES = {0: "waiting_for_wake_word", 1: "connecting", 2: "session_open_press_BOOT", 3: "processing"}
NUMERIC = {
    "device.telemetry": ("state", "uptime_ms", "afe_frames", "mn_frames", "mn_timeouts", "mn_detections", "mn_us", "clean_level"),
    "device.recognition": ("matches", "command_id", "probability", "uptime_ms"),
    "device.recognition_debug": ("matches", "command_id", "probability", "uptime_ms"),
    "device.prompt": ("start_latency_ms", "duration_ms", "uptime_ms"),
    "device.status": (),
}


def numeric(value):
    if type(value) is int:
        return abs(value) <= 2**64
    return type(value) is float and math.isfinite(value)


def sanitize(event):
    """Allow fields by event type; never pass through arbitrary device strings."""
    if not isinstance(event, dict) or not isinstance(event.get("type"), str) or event["type"] not in NUMERIC:
        return None
    kind = event["type"]
    result = {"type": kind}
    for key in NUMERIC[kind]:
        if numeric(event.get(key)):
            result[key] = event[key]
    if kind == "device.telemetry":
        for key in ("vad", "playing", "ptt_pressed"):
            if type(event.get(key)) is bool:
                result[key] = event[key]
        levels = event.get("raw_levels")
        if isinstance(levels, list) and len(levels) == 4 and all(numeric(v) for v in levels):
            result["raw_levels"] = levels
    elif kind == "device.recognition_debug":
        if event.get("phase") in ("speech_end", "timeout"):
            result["phase"] = event["phase"]
    elif kind == "device.prompt":
        for key, choices in (("name", ("wake", "sleep")), ("state", ("started", "completed"))):
            if event.get(key) in choices:
                result[key] = event[key]
    elif kind == "device.status":
        if event.get("kind") not in ("boot", "ready", "wake", "wake_armed", "standby"):
            return None
        result["kind"] = event["kind"]
        source = event.get("source", event.get("message"))
        if source in ("host_test", "ni_hao_xiao_meng", "usb_disconnected", "network_disconnected",
                      "spoken_exit", "button", "remote_stop", "session_idle", "bridge_disconnected"):
            result["source"] = source
    return result


def summarize(events):
    events = [e for raw in events if (e := sanitize(raw)) is not None]
    telemetry = [e for e in events if e["type"] == "device.telemetry"]
    matches = [e for e in events if e["type"] == "device.recognition"
               and e.get("matches", 0) > 0 and e.get("command_id") == 1]
    completed = Counter(e.get("name", "unknown") for e in events
                        if e["type"] == "device.prompt" and e.get("state") == "completed")
    latest = telemetry[-1] if telemetry else {}
    return {
        "local_keyword_matches": len(matches),
        "match_probabilities": [e["probability"] for e in matches if "probability" in e],
        "host_test_wakes": sum(e.get("kind") == "wake" and e.get("source") == "host_test" for e in events),
        "prompt_completions": dict(completed),
        "wake_rearmed_events": sum(e.get("kind") == "wake_armed" for e in events),
        "telemetry_records": len(telemetry),
        "latest_state": STATES.get(latest.get("state"), "unknown"),
        "latest_telemetry": latest,
        "limits": [
            "No PCM or recognized text collected; no acoustic failure rate without time-marked human attempts.",
            "A host_test wake and a prompt completion alone do not prove acoustic recognition.",
            "State 0 permits detection but does not prove model readiness or absence of a prompt tail guard.",
            "State 2 is an open push-to-talk session; the current firmware intentionally disables wake recognition there.",
            "Host arrival times include USB buffering and must not be reported as acoustic response latency.",
        ],
    }


def read_events(path):
    with path.open(encoding="utf-8-sig") as source:
        for line in source:
            try:
                value = json.loads(line)
            except (ValueError, UnicodeDecodeError):
                continue
            if (event := sanitize(value)) is not None:
                yield event


def capture(port, duration, sink):
    import serial

    # Opening the port must not reset the device. No write/heartbeat/wake command,
    # cloud connection, audio capture or board configuration is performed.
    connection = serial.Serial(port=None, baudrate=921600, timeout=.1, write_timeout=1)
    connection.dtr = False
    connection.rts = False
    connection.port = port
    connection.open()
    buffer = bytearray()
    started = time.monotonic()
    try:
        while time.monotonic() - started < duration:
            buffer.extend(connection.read(min(max(connection.in_waiting, 1), 8192)))
            while b"\n" in buffer:
                raw, _, buffer = buffer.partition(b"\n")
                try:
                    value = json.loads(raw)
                except (ValueError, UnicodeDecodeError):
                    continue
                # Drain startup USB backlog before accepting diagnostic events.
                if time.monotonic() - started >= 2 and (event := sanitize(value)) is not None:
                    sink({"host_elapsed_s": round(time.monotonic() - started, 3), **event})
            if len(buffer) > 131072:
                buffer.clear()
    finally:
        connection.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--port", default="COM9")
    mode.add_argument("--replay", type=Path, help="summarize an existing JSONL log without opening serial")
    parser.add_argument("--duration", type=float, default=90, help="seconds, including 2 s USB backlog warmup")
    parser.add_argument("--output", type=Path, help="JSON report; sanitized events saved beside it")
    args = parser.parse_args()
    if not math.isfinite(args.duration) or not 5 <= args.duration <= 3600:
        parser.error("duration must be between 5 and 3600 seconds")
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    output = args.output or ROOT / ".local" / ("wake-diagnostic-" + timestamp + ".json")
    output.parent.mkdir(parents=True, exist_ok=True)
    events_path = output.with_suffix(".events.jsonl")
    if output == events_path or output.exists() or events_path.exists():
        parser.error("choose a new output path; existing evidence is never overwritten")
    events = []
    interrupted = False
    with events_path.open("x", encoding="utf-8") as out:
        def save(event):
            events.append(event)
            out.write(json.dumps(event, ensure_ascii=False, allow_nan=False) + "\n")
            out.flush()

        try:
            if args.replay:
                for event in read_events(args.replay):
                    save(event)
            else:
                print(f"Reading {args.port} for {args.duration:g}s; no device writes or cloud connection.", flush=True)
                capture(args.port, args.duration, save)
        except KeyboardInterrupt:
            interrupted = True
    report = {"mode": "replay" if args.replay else "read_only_serial", "interrupted": interrupted,
              **summarize(events)}
    with output.open("x", encoding="utf-8") as out:
        json.dump(report, out, ensure_ascii=False, indent=2, allow_nan=False)
    print(json.dumps({"report": str(output), **report}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
