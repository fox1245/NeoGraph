# Measuring Program costs

`bench_program_cost` and `scripts/run_program_costs.py` provide a diagnostic
baseline for deciding which Program costs to optimize. They do not change
runtime semantics and do not replace the preregistered QuickJS performance gate.
No LLM, network model call, or model credential is needed.

For an implementation comparison, `scripts/compare_program_costs.py` alternates
two frozen `bench_program_cost` binaries in fresh process pairs. The first such
comparison is documented in [Command publication head optimization](PROGRAM_COMMAND_HEAD_OPTIMIZATION.md).

## Build and run

Use an optimized build on a native filesystem. For example, on Linux:

```sh
cmake -S . -B build-cost -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DNEOGRAPH_BUILD_TESTS=OFF \
  -DNEOGRAPH_BUILD_BENCHMARKS=ON -DNEOGRAPH_BUILD_PROGRAM=ON \
  -DNEOGRAPH_BUILD_QUICKJS_CONTROL=ON \
  -DNEOGRAPH_BUILD_SQLITE=ON -DNEOGRAPH_BUILD_POSTGRES=ON
cmake --build build-cost -j4 --target \
  bench_program_cost bench_quickjs_primitives bench_quickjs_control
python3 scripts/run_program_costs.py \
  --build-dir build-cost --output-dir /tmp/neograph-cost-results
```

The runner requires a **new** output directory. Its default matrix uses memory
and SQLite, seven independent process repetitions, ten warmup invocations, and
100 measured invocations per lifecycle case. `--iterations` and `--warmup`
control lifecycle/direct cases; generator microbenchmarks use 1,000 commands.
Cases run serially in forward/reverse order on alternating repetitions.
SQLite files live in the output directory and remain available for inspection.

To include PostgreSQL, first start a dedicated local test container, set
`NEOGRAPH_COST_POSTGRES_URL` to its published localhost connection URL, and add
`--postgres-container <container-name>`. The runner checks the published port.
It creates a uniquely named database for each sample and drops only that
database afterward. It does not start or stop containers and does not use or
drop the database named in the URL. The disposable server must allow `postgres`
to create/drop databases through `docker exec`. Do not target an application
server. Credentials are not written into metadata or command records.

## What each scope includes

| Case | Included | Excluded |
| --- | --- | --- |
| `direct` | Warm `GraphEngine::run`, 3 sequential increment nodes, 2 state channels | Program admission, journal, checkpoint persistence, JS |
| `lifecycle --mode cpp` | Admitted C++ builder v1 Program, real Core, result construction, journal and checkpoints | JavaScript control execution; compilation/admission are separate cold fields |
| `lifecycle --mode javascript` | Admitted `define()` + generator `main()`, real Core, JS/C++ conversion, command journal and checkpoints | Compilation/admission are separate cold fields |
| `generator` | Generator open/compile/init, first `next`, repeated host-command round trips, terminal conversion, teardown | Real Core, scheduling, journal, database I/O; responses are synthetic |
| `resident` | Multiple independent QuickJS runtimes suspended after their first command | Full Program agents, catalogs, engines and journal storage |
| Existing primitive/control cases | The scopes already documented in `bench_quickjs_primitives` and `bench_quickjs_control` | These samples are not an enabled/disabled gate run |

The same three-node topology, increment behavior, and payload channel are used
in direct, C++ and JavaScript runs. Each real run must finish with counter `3`
and its complete unchanged payload. Payload sizes are 0, 4,096 and 65,536 bytes.
The JavaScript multi-command rows execute the same Core graph 4 or 16 times,
resetting the counter input to zero on every call. They finish with counter `3`.

C++ v1 admits exactly one Program operation. The JavaScript generator uses a
host-declared 64-operation ceiling to support all command-count rows. Both use
the same remaining resource ceilings and one scheduler thread. Their source
forms and command-journaling semantics differ: subtracting C++ from JavaScript
does **not** isolate interpreter time.

Each lifecycle case constructs ProgramStore, CheckpointStore and
ProgramTransitionStore from the selected backend. Memory mode retains real
in-memory journal/checkpoint values; it does not bypass persistence interfaces.
SQLite uses one database file with independent store connections. The existing
CheckpointStore sets WAL and `synchronous=NORMAL`; Program store connections
retain their SQLite synchronous default. PostgreSQL uses the existing store
implementations and a four-connection checkpoint pool. Record the actual
server settings when comparing results. These are backend defaults, not an
assertion of identical power-loss guarantees.

## Reading timing and memory fields

`samples` contains every measured invocation; `first_run` is recorded separately
before warmup is complete. `cold` contains store construction, Program
compilation, admission and Runtime construction. These fields exclude some
fixture setup and therefore are not a complete process-startup partition.

Within one sequential invocation, `before_core_us`, `core_span_us`,
`after_core_us`, and `wake_us` partition start-to-wait-return wall time using
Program event timestamps. **The Core event span is not pure Core CPU time:** it
includes checkpoint work and, in multi-command rows, the intervals between Core
calls. `start_us` and `post_start_us` provide a second partition. Never add the
two partitions together; Core work can begin before `start()` returns.

The event sink records atomic timestamps. Lifecycle results therefore include
this observer, while the direct/generator cases have no event sink. These are
diagnostic timings, not an assertion of observer-free minimum latency.

The runner summarizes each process first, then summarizes those independent
process results. `invocation_median.total_us.median` is the median of process
medians. `invocation_p95.total_us.median` is the median of within-process p95s.
Percentiles use nearest rank; with seven process samples the process-level p95
is their maximum. Raw samples, MAD and extrema remain available. Do not infer
statistical significance from a small difference between medians.

Linux RSS is process memory, not JS allocation accounting. Lifecycle RSS growth
includes retained run history and allocator behavior. Resident-generator RSS
includes runtime/bootstrap overhead and allocator granularity; use the slope
between larger generator counts to estimate incremental occupancy. Neither is
a measurement of total memory per recursive agent. Zero memory fields on other
platforms mean that the Linux-only probe is unavailable.

The runner records binary/source hashes, source commit and tracked diff hash,
CMake options, CPU and process affinity. Keep the raw evidence alongside any
report and do not rebuild binaries during a measurement run.

## Optional database API diagnostic on Linux

`benchmarks/program_store_profile.c` can count and time database-library calls
without changing the Program runtime. Build it separately **after** the main
unprofiled matrix has completed:

```sh
cc -O2 -shared -fPIC -I/usr/include/postgresql \
  benchmarks/program_store_profile.c -o /tmp/program_store_profile.so -ldl -pthread
NEOGRAPH_COST_STORE_PROFILE=/tmp/new-store-profile.json \
LD_PRELOAD=/tmp/program_store_profile.so \
  build-cost/bench_program_cost --case lifecycle --backend sqlite \
  --storage /tmp/new-cost-profile.sqlite --iterations 40 --warmup 0
```

The profiler records only categories and aggregate counts/time, never SQL text,
parameters or credentials. It measures `sqlite3_step`, or synchronous `PQexec`
and `PQexecParams`, and `sqlite3_exec` (including transaction control).
Nested SQLite steps inside `exec` are excluded to avoid double counting.
SQLite row iteration may call `step` multiple times for one
query. Async libpq calls, connection establishment, SQLite prepare time and
caller-side serialization are outside these API intervals. Whole-process
totals include constructors, schema creation, admission and warmup. Profiling
adds overhead, and concurrent API intervals can overlap, so these values must
not be subtracted from baseline wall time as an exact CPU partition. Compare
fresh-database runs with different invocation counts to separate fixed setup
call counts from per-run calls.

## Boundaries of this baseline

This matrix measures sequential synthetic execution and suspended generator
occupancy. It does not qualify tenant fairness, concurrent throughput,
recursive spawn, live replacement, process-loss recovery, production storage,
or a JIT backend. Generator replay rows replay recorded synthetic command
responses; they do not time full durable reconnection. JIT potential requires
an actual comparison that includes warmup, compilation and memory costs.
