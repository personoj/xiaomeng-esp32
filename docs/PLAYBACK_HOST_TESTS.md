# Offline playback checks and ring-write change

## Scope

`main/playback.c` now stages each decoded `audio.delta` into free ring cells and
publishes its write position once, rather than entering/exiting the ISR critical
section for every converted output sample. The 24 kHz source-to-16 kHz output
conversion, source-sample credits, conservative full-buffer check, cancellation,
ACKs, and end-of-stream tail writes are unchanged.

The same reservation lock also prevents a pre-existing counter-rollover bug:
`192000` does not divide `2^32`, so allowing `uint32_t` ring positions to wrap can
map a new write onto an unread physical cell. When `wr` enters its final
ring-capacity range, a whole-ring multiple is subtracted from both counters before
another chunk is staged.
This preserves occupancy and physical indices. The conditional is normally false;
its normal-path cost is included in the candidate benchmark. At continuous 16 kHz
output, unnormalized counters would roll over after approximately 74.6 hours.

### Concurrency argument

1. `playback_event` holds `guard` throughout reservation, conversion and publication.
   All other task-side ring writers, cancellation/compaction and response-slot reuse
   use the same mutex. An ISR does not acquire this task mutex.
2. A short ISR-lock critical section snapshots `wr` and available capacity from
   `rd`, normalizing before any counter can roll over. The existing
   `source_sample_count + 4 <= free_samples` check remains
   conservative for the conversion and its carried one/two-sample tail.
3. Only the free cells after the published `wr` are written. The DMA ISR consumes
   cells before that `wr`; it may advance `rd`, increasing free space. It cannot
   see the staged cells and cannot overwrite them.
4. Publication assigns `wr` under the same ISR lock, after the sample writes. The
   real ESP-IDF critical-section primitive provides cross-core synchronization;
   the host test substitutes do not prove hardware memory ordering.
5. `sent()` and its DMA descriptor credits are not changed. A sample moved to DMA
   is still only queued. Its source credit reaches `played` on the subsequent
   completion callback for that descriptor, never on reception or publication.

The entire new chunk becomes visible after conversion, rather than progressively
sample by sample. This removes repeated interrupt exclusion, but on-device first
sample timing and underrun behavior still need measurement, especially for unusually
large chunks. No device-latency improvement is inferred from the host CPU numbers.

## Run with existing dependencies

From this repository, using the sibling project environment:

```sh
../.venv/bin/python scripts/test_native.py
ASAN_OPTIONS=detect_leaks=0 ../.venv/bin/python scripts/test_native.py --sanitize
```

The second command disables LeakSanitizer only when needed by a ptrace-based
container. AddressSanitizer and UndefinedBehaviorSanitizer stay enabled. Outside
such a container, omit `ASAN_OPTIONS` to retain leak checking.

Requirements: Python 3.10+, a little-endian host, an existing GCC/Clang-compatible
C compiler, and cJSON. The runner first uses an existing ESP-IDF managed cJSON
source, then an installed system runtime. Explicit `--cjson-source`,
`--cjson-include` and `--cjson-library` paths are also supported. If only the cJSON
1.x runtime is installed, `tests/host_cjson/cJSON.h` supplies declaration-only
public ABI bindings. The actual parser/object implementation remains the installed
library. No package is installed or downloaded by the runner.

On Windows with the existing Visual Studio and ESP-IDF managed dependencies, the
original `scripts/test_native.ps1` runs the same expanded test suite. Its existing
cJSON source requirement remains. This cloud review did not run Windows or build
ESP-IDF firmware.

Both runners combine the production `main/playback.c` and `main/main.c` unchanged
except for replacing their includes and embedded-PCM linker annotations. They link
production `main/ptt_capture.c`. RTOS/driver calls, clocks, embedded prompts and
transport sends are deterministic host substitutes. A small test-only Base64
decoder replaces the old always-failing stub; literal wire fixtures now enter the
real `terminal_receive -> playback_event` conversion path. These checks do not test
the mbedTLS implementation or its decoder performance.

### Checks

- Existing wake/sleep cues, PTT gating, quiet tail, late-chunk ACK/discard and silent
  disconnect behavior.
- Known literal PCM wire payload and 77 source-length/chunk-split combinations,
  including signed averaging and one/two-sample `audio.end` tails.
- Physical ring wrap, tail wrap and single-owner DMA descriptors across responses.
- Queued is not played: no playback-start or drain event until the DMA descriptor
  completes; exact source-credit totals and one drain report.
- Selective cancel/compaction, already-queued DMA zeroing, rejected late chunks and
  slot reuse without inherited DMA credit.
- Existing conservative full-buffer boundary/interrupt behavior and duplicate,
  done, invalid-Base64 and odd-byte chunk rejection/ACK behavior.
- An ISR injected just before publication sees only old `wr`. A nearly-full-ring
  variant checks all unread cells survive staging and DMA consumption.
- Deterministically varied chunk sizes and interleaved DMA completions preserve
  exact source credit totals.
- A nearly-full ring just before `uint32_t` rollover preserves unread data, DMA
  credit and selective cancellation/compaction after normalization. The comparison
  script also runs this fixture against the baseline and records its expected
  unread-cell preservation failure.

These are deterministic interleaving tests, not simultaneous-thread stress tests
or ESP32 acoustic/real-time acceptance.

## Reproduce baseline and candidate comparison

```sh
../.venv/bin/python scripts/benchmark_playback.py \
  --baseline-root ../baselines/xiaoyun-esp32 \
  --output-dir ../evidence/latency/esp32-playback --repeats 5
```

The baseline is the untouched published snapshot, commit
`963d24057bbe7b0144b2c878323bc07d30473fc0`. Both binaries use the same current host
fixtures. Baseline skips the new single-publication guarantee and rollover fix in
its positive suite. Common behavioral tests run for both; a separate rollover
regression is expected to fail on the baseline and pass on the candidate. The
script records source
hashes, compiler/platform information, complete repeated timings and lock counts.
It alternates the order of five before/after runs to reduce warm-up/drift bias.

Each run contains nine batches of 4,000 `playback_event` calls at each chunk size.
JSON input is preconstructed; production JSON lookup, fixture Base64 decode, PCM
allocation/conversion, ACK construction and mocked send are inside the timing.
The ring/response sequence state is reset between calls, with no DMA consumer in
the timed loop. Lock primitives count entries and enforce fixture invariants; they
do **not** model FreeRTOS critical-section cost. Timings are means of process CPU
nanoseconds per event within a batch. Their distribution is not device packet
latency or an end-to-end p95/p99.

For an already-known response and complete groups of three input samples:

| Source samples | Output samples | Baseline task-lock entries | Batched entries |
| --- | --- | --- | --- |
| 96 | 64 | 65 | 2 |
| 960 | 640 | 641 | 2 |
| 3840 | 2560 | 2561 | 2 |

Creating a response adds one lock entry to either path. At most two remaining
`audio.end` tail writes retain the old per-sample locking. Actual CPU timings are
recorded in the comparison JSON, rather than treated as a firmware speed claim.

### Recorded cloud-host result, 2026-10-11

Debian GCC 14.2.0, `-O2`, cJSON 1.7.18. Five alternating pairs produced 45 batches
of 4,000 events per source size and version. Other project workers confirmed their
CPU-heavy jobs had stopped before this final run; an earlier overlapping audit
run was explicitly excluded. External host scheduling/frequency variation remains
possible.

| Source samples | Baseline median CPU/event | Candidate median CPU/event | Reduction |
| --- | --- | --- | --- |
| 96 | 1.0825 microseconds | 0.9920 microseconds | 8.36% |
| 960 | 7.53825 microseconds | 6.79225 microseconds | 9.90% |
| 3840 | 29.39175 microseconds | 25.43625 microseconds | 13.46% |

The baseline and candidate checksums match. The baseline fails the isolated
rollover fixture at unread sample preservation; the candidate passes, including
subsequent DMA consumption and selective cancellation. Both pass the common
behavioral suite under AddressSanitizer and UndefinedBehaviorSanitizer; the
candidate also passes publication/normalization regressions. LeakSanitizer was
disabled because this execution container uses ptrace.

Evidence is in sibling `evidence/latency/esp32-playback/playback-comparison.json`,
with build/test logs and expected-failure output alongside it. Sanitized logs are
`evidence/latency/esp32-native-sanitized.txt` and
`evidence/latency/esp32-native-baseline-sanitized.txt`. These host microseconds do
not measure device first-sound latency, FreeRTOS cost, or end-to-end latency.

## Still requires the local/device reviewer

Without authorizing flashing or connecting a device in this review:

1. Inspect the mutex/ISR publication assumptions against the target ESP-IDF build
   and board's PSRAM/cache configuration; compile firmware with its pinned setup.
2. When separately authorized, compare the same streamed PCM before/after,
   including ring wrap, backpressure, BOOT interruption, late deltas and reconnect.
3. Confirm audible output/underruns, real first-play timing, and DMA-completion
   `playback.progress` sample counts using existing device telemetry. Check long
   and very large chunks as well as normal bridge chunks.
4. Keep hardware/firmware observations separate from the host benchmark. Cloud
   model, physical execution and browser-visible latency were not measured here.
