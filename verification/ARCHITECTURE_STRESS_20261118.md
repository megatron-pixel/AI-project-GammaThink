# Architecture stress results, November 18

**The defined workloads passed. No incorrect proof, silent corruption or
committed-history loss was observed.** The main exposed limitation is bounded
search capacity, not RAM exhaustion. This is a finite synthetic stress test,
not a claim that every workload or English interpretation is correct.

## Integrated application: larger archive and history

The existing application probe ran with OS network access denied, the actual
saved LFM control Q4, 100,000 original 2 KiB messages, 200,000 immutable 2 KiB
archive objects and 100 project documents. This is ten times the earlier
history and twice its archive record count.

| Measurement | Actual |
|---|---:|
| Original historical messages | 100,000 |
| Immutable records compiled and individually streamed | 200,000 |
| Stream pages | 3,125 |
| Canonical archive input bytes | 459,977,780 |
| Stored snapshot bytes | 118,957,006 |
| Project documents / source bytes | 100 / 1,638,400 |
| Whole-process lifetime peak RSS | **1,148,862,464 bytes** |
| RSS after model unloading | 200,687,616 bytes |
| Elapsed time, including seeding and compilation | 451.825 seconds |
| Actual prepared model input | 3,140 tokens |
| Neural output | One unadmitted token |
| Network capabilities | Zero, additionally denied by the OS |

The 730,899,040-byte Q4 has 1,170,340,608 retained parameters. It loaded,
performed actual CPU prefill, and unloaded before native proof and full archive
streaming. The existing 3,000,000,000-byte process limit stayed unchanged.
Checked response delivery succeeded after controller/database reopening with
model loading forbidden. The generated neural fragment was never admitted.
The formal task was an explicit structured control, not free-flow translation.

The large archive payload is repetitive and compressible. A second workload
therefore used **8,192 high-entropy 8 KiB payloads**, totalling 67,108,864 raw
payload bytes. All records survived exact identity-checked streaming. Canonical
input was 68,728,660 bytes and stored snapshot size was 71,207,686 bytes:
incompressible content and indexes can make the snapshot larger, not smaller.
This run took 44.727 seconds and peaked at 23,150,592 process bytes.

An undersized snapshot input budget rejected explicitly. Missing index blocks
returned an unavailable error, and bit-corrupted index bytes returned an
integrity error. Neither became an ordinary missing-object answer.

## Reasoning: independent oracle, cycles and hard bounds

The optimized probe exercised **8,000 queries across 1,000 seeded signed-Horn
modules**, each containing 32 symbols and 128 rules. Sparse facts, denser
facts, fact-free cyclic graphs and random mixtures were included.

An independent monotone fixed-point oracle supplied expected complete
outcomes. Every native reply passed request-bound certificate checking and
exact binary round-trip checks. Retained positive and negative proof roots
were checked against the oracle even when the search was limited.

| Observed result | Count |
|---|---:|
| PROVED | 1,013 |
| DISPROVED by explicit signed evidence | 1,009 |
| UNKNOWN | 4,879 |
| CONTRADICTION, retaining both signed roots | 1,099 |
| Total queries | 8,000 |
| Searches reaching a depth/event bound | **3,436** |
| Complete searches matching the oracle | **4,564** |

The limited-search count overlaps the outcome rows; it is not an additional
category. None of those limited replies became a conclusive certificate.
No missing evidence was converted into an explicit negative fact.

The optimized run passed 28,623 checks in 38.464 seconds at 17,121,280 peak
process bytes. Additional cases verified:

- Chains of 8, 32, 64, 127 and 128 rule links proved successfully.
- Chains of 129, 256 and 511 links stopped with an explicit depth limit.
- Sixteen repeated joint prerequisites remained sixteen proof dependencies.
- Cycles with an alternative fact and an explicit contrary fact retained both
  contradiction roots.
- Exact 1,024-symbol and 512-rule module boundaries were admitted; one-over
  symbol, rule and sixteen-premise bounds rejected.
- Event exhaustion was reported; invalid event limits and truncated wire
  inputs rejected.
- Duplicate-heavy 64-request batches at one, two and four workers produced
  byte-identical replies to serial execution: 32 executions and 32 memo hits.
  The 64 MiB reply-envelope reservation was explicit, not an increased
  per-proof search limit.

**Practical limit:** current native searches allow depth 128 and 4,096 events.
Dense/cyclic tasks can exhaust that budget on a small module. Storing 200,000
records does not imply that one proof can reason over all of them at once.
These limited outcomes were expected safety behavior, not defects repaired
by raising limits or changing expected answers. This run did not compare the
separate forward-reasoning strategy on the same random cases.

## Memory and undefined-behavior instrumentation

All seven original reasoning-core source files were rebuilt with
AddressSanitizer and UndefinedBehaviorSanitizer. The same 8,000-query and
boundary/parallel workload passed the same 28,623 checks with no sanitizer
finding. Observed peak RSS was 469,286,912 bytes and elapsed time was
26.668 seconds. These separate timing samples are not a speed comparison.

Auxiliary component archives were not rebuilt with instrumentation.
This is core sanitizer coverage, not whole-application instrumentation or
ThreadSanitizer/data-race qualification.

## Actual killed-writer recovery

Sixteen independent fixture writers each committed 128 original messages,
two immutable summary versions and two pin revisions. Each then performed a
real conversation/FTS append inside an uncommitted transaction and explicitly
flushed dirty pages into its WAL.

The parent subscribed to an exact marker before sending START, observed
16,512 uncommitted WAL bytes, and sent SIGKILL only after the flushed marker.
No sleep or timing-based success condition was used.

All sixteen fresh recovery processes verified:

- All **2,048 committed originals** across the fixtures remained byte-correct.
- No uncommitted message or FTS entry survived.
- Historical and current summary/pin revisions remained intact.
- A new append received ID 129 and was immediately searchable.
- SQLite database and external-content FTS integrity checks passed.

Every owned writer exited with SIGKILL/137 as intended; every recovery process
exited 0. This tests process death during a flushed uncommitted transaction,
not sudden power loss, damaged hardware or a mid-commit filesystem failure.

## Evidence and scope

Full protocol, isolated harnesses, compiler arguments, all process receipts,
source-version hashes and measured results:
`../../.omo/evidence/architecture-stress-20261118/RESULT.json`.

All stress workloads exited 0; the deliberately killed fixture writers are
the explicit exception. C++ diagnostics and strict warning-enabled compilation
were clean. No production reasoning algorithm, original architecture, model
weight, sealed evaluation or user database was changed. No implementation
defect requiring a production fix was found in these runs.

These tests do not establish an actual 8 GB host, aggregate whole-host or
remote-provider RAM, sustained neural generation, broad English fidelity,
whole-library meaning, live IPFS availability or frontier-level intelligence.
