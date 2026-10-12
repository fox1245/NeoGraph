# Join model (#363)

**Languages:** [English](README.md) | [한국어](README.ko.md) | [日本語](README.ja.md) | [简体中文](README.zh-CN.md)

`BarrierJoin.tla` models BSP super-steps with one AND join whose `wait_for`
slots may hold several `any_of` alternatives, the approved scope of #363 (join
modes are deferred). `tests/test_join_exhaustive.cpp` states the same rules as
a reference interpreter and checks the real `Scheduler` and `GraphEngine`
against it on larger graphs. Keep the two in step when the semantics change.

## Rules

- A slot is filled when any of its alternatives signals the join. The join
  fires when every slot is filled, and then all accumulated signals clear.
- Signals from non-members are recorded and never count.
- Conditional routes are nondeterministic label choices.
- `Command.goto(__end__)` preempts every route of its super-step.
- When nothing is ready, the run ends. It is stalled, and `Failed`, when the join
  still holds signals (from members or not), unless the final super-step (the
  one after which nothing is ready) reached `__end__` by an explicit route: a
  static edge to `__end__` (as `.exit()` writes), a conditional route to
  `__end__`, or `Command.goto(__end__)`. An implicit end (a node without
  successors) never suppresses the stall, and an explicit end in an earlier
  super-step does not either.

So a leftover `any_of` alternative that arrives in the super-step in which the
join exits completes, and one that arrives a super-step later, with nothing
else ready, fails.

The C++ harness also covers failure routes (#364): a node that fails along its failure edge routes to that edge's target only, and `Command.goto` still preempts it. A failure target is never a join or `__end__`, so the rules above hold unchanged; this TLA+ model does not include failure edges.

## Invariants

| Name | Meaning |
|---|---|
| `NoDoubleFirePerRound`, `JoinRunsOnlyWhenFired`, `FireClearsSignals` | Every fire consumes arrivals that fill every slot since the previous fire; the join runs only right after a fire, and a fire clears all signals. |
| `StallImpliesFailed`, `FailedOnlyOnStall` | A run that ends with signals waiting at the join fails, and only such a run fails. |
| `OnlyExplicitEndSuppressesStall` | A run that completes with signals waiting reached `__end__` explicitly in its final super-step. |
| `LeftoverAlternativeStalls` | An alternative that arrives after the join fired, with no later fire and no explicit end in the final super-step, fails the run (fail-closed). |
| `NamedOutcomes` | The two non-exclusive #357 variants: the leftover arriving with the exit completes; arriving one super-step later fails. |
| `LegacyEquivalence` | With singleton slots the slot rule is today's barrier: `wait_for` is a subset of the signals received. |
| `Termination` (property) | Every run completes, fails or reaches the step limit. |

## Running TLC

TLC is not part of CI. It needs Java 11 or later and `tla2tools.jar` from the
TLA+ releases (<https://github.com/tlaplus/tlaplus/releases>):

```sh
cd tests/model
java -XX:+UseParallelGC -cp /path/to/tla2tools.jar tlc2.TLC \
    -workers auto -config BarrierJoin.cfg BarrierJoin.tla
```

`BarrierJoin.cfg` checks every graph with at most three nodes, with every
`any_of` partition of the join's members, plus the #357 graphs named in the
spec (legacy barrier, `any_of` slot, the two non-exclusive variants, an
explicit end by `Command.goto`). TLC finds 335,521 distinct states and no
error; most of the two minutes on eight workers go to computing the 21,707
initial states. `MaxNodes = 4` is out of reach for TLC; the C++ harness covers
up to five nodes.

To see the invariants bite, let an implicit end suppress the stall
(`StallImpliesFailed` fails), or fire the join on any member signal
(`NoDoubleFirePerRound` fails).

## Resume model

`tests/resume_model.h` and `tests/test_resume_model.cpp` check the interrupt,
resume and replay contract of the `GraphEngine` against a reference
interpreter, as the join harness checks joins; `tests/test_program_resume_model.cpp`
does the same for the Program runtime. The reference states the documented
contract (`docs/concepts.md`): a super-step runs its ready nodes, then their
Sends; a step whose invocations ask pauses on all of them and keeps what
finished; a resume answers the interrupt it names, or the first, and that
invocation alone receives the value (#388, #393); an `interrupt_before`
approval reaches the node it held, each gate of a step is answered on its own,
and only an answered gate is passed (#382, #396); a failure outranks an
ordinary interrupt of its step (#392).

### Core harness

- **Family.** Main nodes `n0`..`n{N-1}` with forward edges (at most two
  successors each), every node reachable. A node is `Ok`, `Ask` (raises
  `NodeInterrupt` until answered; with two successors it may route on its
  answer), `Fail` (with or without a failure edge, #364), `Flaky` (fails, then
  a retry succeeds in its own attempt namespace) or `Map` (one or two Sends to
  a worker whose items are `Ok`, `Ask` or `Fail`). Optionally the last node
  with two static predecessors is a barrier join, and one `Ok` or `Ask` node
  carries `interrupt_before` or `interrupt_after` (two `interrupt_before`
  gates in the extended bound).
- **Schedules.** Every answer order of the reference: each pending question by
  its interrupt id and by default, with both labels of an `Ask` that routes on
  its answer. Each schedule also runs with a crash at every durable operation
  (each checkpoint `save` and `put_writes`; before each one and after the last,
  or before and after each one in the extended bound, which also crashes a
  second time during recovery) and with a fork of every pause. A crash stops
  the process: the store refuses every further operation, and the host
  recovers from the latest checkpoint as a real one would. It repeats the
  resume it was executing when that checkpoint is the one it answered,
  surfaces an `interrupt_before` or `interrupt_after` pause it has not seen,
  resumes anything else without a value, and runs again a thread that has no
  checkpoint. Every schedule runs on `InMemoryCheckpointStore`, and crash-free
  (crashes too on graphs of up to two nodes, three extended) on
  `SqliteCheckpointStore` with a coarse clock: every checkpoint saved with one
  timestamp (`tests/fixtures/coarse_clock_store.h`).
- **Asks.** Every schedule runs twice: with asks that take only an answer
  addressed to their own invocation id, and with asks that take any resume
  value, as an approval node does.
- **Properties.** P1 exactly once: no node body completes again after its
  completion was durable (its pending write, or its step's commit); a body
  runs again only after a crash lost its completion. P2 every question
  asked: the pauses and their pending questions with payloads equal the
  reference's, and each `Ask` consumed exactly the answer it was given.
  P3 same result: status, log, failure records and routes equal the
  reference. P4 crash transparency and P5 fork equivalence: P1-P3 hold on every
  crash and fork schedule.

### Program harness

JavaScript `main()` programs (`callCore`, `ng.all`, `ng.race` and `ng.quorum`
of two members run together or one at a time, `ng.await`, and a `callCore`
whose Core step runs two nodes that may both ask, #388 and #394) and typed plans
(`parallel`, a two-of-three `quorum`, `sequence`) over a one-node Core whose
node may call a bound provider and may ask. The host is wired as
`LocalProgramHost` wires one: in memory with the in-process priced broker, or
with every store in one SQLite file, checkpoints saved on the same coarse
clock, and the SQLite provider journal. The
provider is a real SDK client over a local transport that counts the requests
it sends. P1 also covers "sent once, charged once": the requests, the tokens and
the money a run is charged equal the reference's (with the in-process broker, a
node that runs again calls and pays again, as documented). Crashes run on the
SQLite host for programs that run one call at a time: the database is copied
at every checkpoint save, `put_writes`, Program transition, provider dispatch
and journaled call, and a new runtime on the copy reconnects the run.

A call that saved a Core checkpoint continues from it after a crash (#402),
and a fork replays the provider calls its paused invocation made (#405); a
schedule that does otherwise fails. The harness reports, without failing, the
schedules that follow one of two documented behaviours P4 does not cover: a
call that stopped before its first Core checkpoint, in a JavaScript program or
a plan, is returned to the host to reconcile (#402, #404), which the harness
accepts only when the crash left the call without one; and a crash while a
request was out holds that call, which is never sent again (#364). A call the
host can reconcile as completed (a call, an awaited call or a pair that neither
asks nor fails, with no priced call after it) it reconciles with the reference
result, and the run must then end as the reference does (#403).

### Bounds and variables

| | CI bound | `NEOGRAPH_RESUME_MODEL_BOUND=extended` |
|---|---|---|
| Main nodes, special nodes | 3, 2 | 4 (3 special up to three nodes, 2 at four) |
| Answers, crashes per schedule | 4, 1 | 6, 2 |
| Send items | `ok`, `ask`, `ask ok`, `ask ask`, `ask fail` | every pattern of one or two |
| Gates | one, on `Ok` or `Ask` | one on any node, or two `interrupt_before` |

At the CI bound the Core harness runs 2,196 graphs and about 6,100 answer
schedules for each kind of ask, some 83,000 runs each; the Program harness runs
124 JavaScript programs and 18 plans, about 950 schedules. Each test takes
under a minute on four workers.

Variables: `NEOGRAPH_RESUME_MODEL_NODES`, `_SPECIAL`, `_SPECIAL_LARGEST`,
`_ANSWERS` and `_CRASHES` override single limits; `NEOGRAPH_RESUME_MODEL_SQLITE=0`
leaves SQLite out; `NEOGRAPH_RESUME_MODEL_THREADS` sets the workers (default
4). For the Program harness, `NEOGRAPH_PROGRAM_RESUME_MODEL_BOUND=extended`
adds two-command programs, every fork and a crash after every operation;
`_ONLY` keeps the programs whose description contains it, `_CRASHES=0` leaves
crashes out, `_THREADS` sets the workers (default 4), and `_DEBUG` prints every
result and durable operation.
