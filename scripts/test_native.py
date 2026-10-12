#!/usr/bin/env python3
"""Offline GCC/Clang host checks of production playback/main/PTT C sources.

Uses an existing cJSON dependency (source, development library, or system runtime).
It never installs dependencies, contacts a device, or invokes the firmware toolchain.
"""
from __future__ import annotations

import argparse
import ctypes.util
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-root", type=Path, default=ROOT,
                        help="production source root; can point at an untouched baseline")
    parser.add_argument("--build-dir", type=Path, default=ROOT / ".local/native-tests")
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--cjson-source", type=Path,
                        help="existing cJSON.c (otherwise use installed cJSON)")
    parser.add_argument("--cjson-include", type=Path)
    parser.add_argument("--cjson-library", help="existing library path or linker argument")
    parser.add_argument("--sanitize", action="store_true", help="ASan and UBSan")
    parser.add_argument("--benchmark", action="store_true", help="also run a synthetic host benchmark")
    parser.add_argument("--legacy-ring-writes", action="store_true",
                        help="baseline comparison: skip the new single-publication guarantee")
    args = parser.parse_args()
    if sys.byteorder != "little":
        parser.error("These ESP32 PCM fixtures require a little-endian host.")
    cc = shlex.split(args.cc)
    if not cc or not shutil.which(cc[0]):
        parser.error("An existing GCC/Clang-compatible C compiler is required; nothing was installed.")
    source_root = args.source_root.resolve()
    target = args.build_dir.resolve()
    target.mkdir(parents=True, exist_ok=True)

    # Same adaptation as test_native.ps1: production function bodies are untouched.
    parts = []
    for name in ("main/playback.c", "main/main.c"):
        source = (source_root / name).read_text(encoding="utf-8")
        source = re.sub(r"(?m)^#include[^\r\n]*", "", source)
        source = re.sub(r' asm\("[^\"]+"\)', "", source)
        parts.append(source)
    (target / "firmware_under_test.c").write_text(
        '#include "native_mocks.h"\n' + "\n".join(parts), encoding="utf-8")

    vendored = source_root / "managed_components/espressif__cjson/cJSON/cJSON.c"
    cjson_source = args.cjson_source or (vendored if vendored.is_file() else None)
    includes = [ROOT / "tests", target]
    if args.cjson_include:
        includes.append(args.cjson_include.resolve())
    elif cjson_source:
        includes.append(cjson_source.resolve().parent)
    elif Path("/usr/include/cjson/cJSON.h").is_file():
        includes.append(Path("/usr/include/cjson"))
    else:
        includes.append(ROOT / "tests/host_cjson")
    link = []
    if cjson_source:
        link.append(str(cjson_source.resolve()))
    elif args.cjson_library:
        link.append(args.cjson_library)
    else:
        library = ctypes.util.find_library("cjson")
        if not library:
            parser.error("cJSON is missing. Supply an existing --cjson-source or --cjson-library; "
                         "the runner does not download dependencies.")
        if sys.platform.startswith("linux") and not os.path.isabs(library):
            link.append("-l:" + library)
        elif os.path.isabs(library):
            link.append(library)
        else:
            link.append("-lcjson")
    flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Wno-unused-parameter",
             "-Wno-unused-function", "-Wno-unused-variable"]
    if args.sanitize:
        flags += ["-O1", "-g", "-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    if not args.legacy_ring_writes:
        flags.append("-DNATIVE_EXPECT_BATCH_APPEND=1")
    common = cc + flags + ["-I" + str(p) for p in includes]
    for name in ["native_playback", "native_ptt", "native_process_cpu", "native_playback_bench_timer_failure"] + (["native_playback_bench"] if args.benchmark else []):
        binary = target / (name + (".exe" if os.name == "nt" else ""))
        command = common + [str(ROOT / "tests" / (name + ".c"))]
        if name != "native_process_cpu":
            command.append(str(source_root / "main/ptt_capture.c"))
        if name not in ("native_ptt", "native_process_cpu"):
            command += link
        command += ["-o", str(binary)]
        print("+", shlex.join(command), flush=True)
        subprocess.run(command, check=True)
        if name == "native_playback_bench_timer_failure":
            result = subprocess.run([str(binary)], text=True, capture_output=True)
            if result.returncode == 0 or result.stdout or "failed" not in result.stderr:
                raise RuntimeError(f"Expected a clock error and no benchmark output: {result}")
            print("PASS: failed CPU-clock read aborts benchmark without a timing report", flush=True)
        else:
            subprocess.run([str(binary)], check=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
