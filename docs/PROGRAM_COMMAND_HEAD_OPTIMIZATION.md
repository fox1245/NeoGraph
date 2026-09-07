# Command publication head optimization — 2026-09-07

This change reduces reads before publishing a JavaScript command. It reads the run, journal and newest command together instead of loading the run/journal separately and materializing the complete command history. It does not change durable publication, CAS, reservation, checkpoint, or recovery semantics.

## Implementation

- `ProgramTransitionStore::load_command_publication_head` returns a coherent run/journal pair and the newest command append. Memory uses one immutable snapshot; SQLite and PostgreSQL use one statement and the existing `(owner_scope, run_id, sequence)` primary key.
- The database projection omits unused migration and last-publication bytes. The newest command sequence and coordinate are checked against its canonical value; run/owner/bundle/journal bindings are validated.
- Normal append and settlement use the newest command. Older-coordinate retries still use the complete history. Replay and store-side append/reservation validation still inspect history as before; this is not a complete removal of history scans.
- Existing C++ store wrappers get a fallback using their existing virtual reads, fenced by a second run read. Concurrent changes fail closed. Wrappers can implement the new read while preserving their own filtering and authority semantics.
- No schema migration, new index, cache lifetime, durability setting, graph generation, compiler identity, or model configuration changes.
- The public C++ virtual interface gained a method: rebuild the Program library and C++ consumers together. Source compatibility for existing wrappers is covered; prebuilt C++ consumers are not ABI-compatible by assumption.

## Correctness

The WSL GCC Debug Program suite passed **680 tests**, with **2 inapplicable memory-store process-restart cases skipped**. The seven added tests and augmented backend history tests cover immutable snapshots, independent-reader/writer connections, fallback wrappers, torn fallback reads, selected-tail corruption and bounded tail reads. Historical corruption remains rejected by the full-history read.

The complete suite also exercised command recovery, lineage CAS, recursive child replacement, nonrenewable budgets and real process-loss boundaries on SQLite/PostgreSQL. No Windows or sanitizer rerun is claimed for this change.

## Paired Release comparison

Same Ryzen 7 5800X / WSL2 Ubuntu 24.04 / GCC 13.3 Release configuration as the baseline. Both binaries were frozen before the comparison. Five independent process pairs per case; each process uses 3 warmup and 12 measured invocations. Cases and AB/BA order alternate. PostgreSQL uses native WSL Docker and a new database per process; SQLite uses a new WSL-ext4 file. No builds or other test jobs ran during the paired matrix.

The before binary hash matches the original baseline. The current before/after measurements below supersede comparisons against old wall-clock timings; they were measured together in this turn. No JIT or LLM is involved.

| Case | Before median (ms) | After median (ms) | Median paired reduction | Paired after/before range |
| --- | --- | --- | --- | --- |
| direct-0 | 0.0076 | 0.0077 | -2.71% | 1.0007–1.0418 |
| memory-cpp-0 | 2.0373 | 2.0332 | -0.58% | 0.9857–1.0336 |
| memory-javascript-0-commands-1 | 3.3096 | 3.3046 | +0.59% | 0.9780–1.0432 |
| memory-javascript-65536-commands-1 | 31.0390 | 30.7436 | +0.89% | 0.9843–1.0272 |
| memory-javascript-0-commands-16 | 32.0270 | 32.2163 | -0.13% | 0.9808–1.0261 |
| sqlite-javascript-0-commands-1 | 19.5180 | 18.9032 | +3.20% | 0.9645–1.0497 |
| sqlite-javascript-65536-commands-1 | 130.1083 | 122.0217 | +6.02% | 0.9213–0.9490 |
| sqlite-javascript-0-commands-16 | 326.5041 | 256.5506 | +20.94% | 0.7812–0.8014 |
| postgres-javascript-0-commands-1 | 81.8919 | 78.3417 | +4.34% | 0.9037–1.0263 |
| postgres-javascript-65536-commands-1 | 257.4166 | 240.3124 | +6.76% | 0.9198–0.9608 |
| postgres-javascript-0-commands-16 | 1035.1816 | 922.3063 | +10.90% | 0.8689–0.9304 |
| postgres-cpp-0 | 49.0719 | 48.2794 | +1.48% | 0.9555–1.0062 |

Reduction is computed from paired process medians, so it need not equal the ratio of the two independent summary medians. Small differences within the paired spread are inconclusive. Direct Core and C++ Program rows are negative controls; the optimized command-publication read is not used there. The small-payload memory case does not establish a clear speedup.

The small direct-Core signal prompted one wider control check: seven paired
processes, 100 warmup and 10,000 measured direct invocations each. Its median
after/before ratio was 1.0098, with pairs ranging from 0.9825 to 1.0123. Raw
results are retained under `direct-control/`. No Core source or library code was
changed; this check does not establish a meaningful Core performance change.

## Database API counts

A separate instrumented comparison used one versus four invocations and repeated the difference twice. Every per-run count difference was identical across the two repetitions. Timings from this instrumentation are not used for the headline comparison. SQLite step calls include row iteration; libpq covers synchronous calls only.

| Backend / Core calls per Program run | Before SQL API calls | After SQL API calls | Before commits | After commits |
| --- | --- | --- | --- | --- |
| sqlite / 1 | 135 | 130 | 9 | 9 |
| sqlite / 16 | 2715 | 2155 | 114 | 114 |
| postgres / 1 | 108 | 104 | 4 | 4 |
| postgres / 16 | 1053 | 989 | 34 | 34 |

The PostgreSQL commit column is only the synchronous libpq subset; it is not the total transaction count of the async checkpoint path. Both binaries run the same persistence interfaces and keep the same commit counts in the measured scopes.

## Reproduction and evidence

Build `bench_program_cost` from the before revision (`8fce5e14`) and the changed revision with identical Release options. Preserve both binaries. With the disposable Postgres container running and `NEOGRAPH_COST_POSTGRES_URL` set:

```sh
python3 scripts/compare_program_costs.py \
  --before /tmp/before/bench_program_cost \
  --after /tmp/after/bench_program_cost \
  --output /tmp/new-command-head-comparison \
  --postgres-container neograph-n2-postgres
```

Detailed method: [Program cost measurement](PROGRAM_COST_MEASUREMENT.md). Raw paired samples, counts, source/binary hashes and verification logs are retained locally in `artifacts/program-command-head-20260907/`; large SQLite files remain in WSL. The runner does not overwrite output directories or reuse existing application databases.

The next remaining target is full-history validation/materialization during publication and replay, plus canonical record construction. This change does not demonstrate that those costs are eliminated or that a JIT would help.
