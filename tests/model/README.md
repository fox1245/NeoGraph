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
