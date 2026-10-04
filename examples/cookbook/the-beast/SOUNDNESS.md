# Soundness of the Coherence Gate — a formal companion

**Languages:** [English](SOUNDNESS.md) | [한국어](SOUNDNESS.ko.md) | [日本語](SOUNDNESS.ja.md) | [简体中文](SOUNDNESS.zh-CN.md)

## Typed provider migration — evidence boundary

The migrated C++ author/repair paths build typed `ProviderRequest` and `sp::Message` histories and consume full immutable `sp::Outcome` results, including every returned native message/part. Text extraction is only for parsing the candidate JSON/Python; it is not used to replace native conversation history. Compiler/parse/initialization feedback is appended after those messages. Semantic repair is bounded by each program's attempt limit (live author: three; Forge server generation: two); it is not transport retry or unlimited evolution.

The interface-4 [Forge source contract](README.md) separately bounds empty
`MaxTokens` recovery to one doubled-cap call with a pinned 300-second deadline
and retained outcomes/usage; it never retries failures or observer/settlement errors.

Keep the private OpenRouter route `~deepseek/deepseek-v4-flash-latest` with `zdr: true`, `only: ["morph"]`, `allow_fallbacks: false`: no eligible endpoint means failure, not disclosure to another provider. ZDR and historical US endpoint listings are not a residency guarantee or proof of current availability. Live execution requires an authorized key, network/provider availability and paid capacity; prompts, exported schemas, diagnostics and native history are sent to that route. Generated Python server execution has its own local code-execution trust boundary, separate from NeoGraph's Python provider bindings. Never publish keys or private prompts. Nullable token counters are not a dollar cost; pricing and actual billable usage are prerequisites.

Preserved interface-3 scoped runtime evidence covers actual offline strict Core compilation, evolution and checkpoint rollback. It does not qualify every live/apex/forge/script variant or vendor inference. Retained console transcripts, benchmark/fuzz results and live execution claims below remain historical, not interface-4 measurements. Coherence gates establish their stated structural/effect-contract properties, not provider transport/privacy correctness or model semantic truth. The Python provider bindings now follow the typed API; their verification is separate from these C++ runs.

This is the theory behind the empirical harnesses in this cookbook. `gate_eval`
*measured* the coherence gate sound on a labeled corpus; `gate_fuzz` *measured*
it over thousands of mutants and mapped its boundary (sound relative to honest
effect contracts, with a runtime backstop for dishonest ones). This document
*proves* the corresponding theorem over a small-step operational semantics of
harness execution and the effect lattice the gate reasons about.

The proof concerns the abstract machine below, with total reducers, returning
node bodies, fixed declared channels and total, statically routed dispatch. It excludes
node/reducer exceptions, cancellation, provider/tool failures, resource limits,
checkpoint/custody errors and arbitrary generated code. The current C++ engine
has these additional failure modes. The cited implementation files motivate the
model; the historical corpus does not mechanically verify the C++ implementation
or establish freedom from undefined behavior outside this model.

Notation is ASCII: `⊆ ∪ ∩ ∅` are set operations; `⟨…⟩` a machine configuration;
`↦` a map entry; `→` the step relation; `⊢ G ok` the well-formedness judgment.

---

## 1. Syntax

A harness (compiled graph) is

    G = (N, E, D, ρ, R)

- `N`      — finite set of node names. Two distinguished names `⊥s = __start__`
             and `⊥e = __end__` are not in `N`.
- `E ⊆ (N ∪ {⊥s}) × (N ∪ {⊥e})` — static edges.
- `D`      — finite set of **declared channels** (the `channels` block).
- `ρ : N → 𝒫(Chan) × 𝒫(Chan)` — the **effect contract** of each node, written
             `ρ(n) = (rd(n), wr(n))`: the channels the node declares it reads and
             writes. `Chan` is the universe of channel names; `D ⊆ Chan`.
- `R`      — routing data for conditional nodes: for a conditional node `n`,
             `R(n)` is a partial map from labels to targets (the route map), plus
             an optional declared `ConditionSpec` fixing its label set.

Barriers (AND-join nodes) carry a `wait_for ⊆ N` set. We fold these into `G`
where needed.

## 2. The effect lattice

Effects live in the powerset lattice over channels

    L = (𝒫(Chan), ⊆, ∪, ∩, ∅, Chan)

with join `∪`, meet `∩`, bottom `∅`, top `Chan`. Two facts we use:

- **Composition is join.** The write-effect of running a set of nodes `F` in one
  super-step is `⋃_{n∈F} wr(n)` — the lattice join of their write-effects.
- **Monotonicity.** If `wr(n) ⊆ D` for every `n`, then `⋃_{n∈F} wr(n) ⊆ D` for
  every `F ⊆ N` (join preserves the upper bound `D`).

`D` is the relevant element of `L`: the set of channels the state can hold.

## 3. Small-step operational semantics

A **configuration** is `⟨F, σ⟩` where `F ⊆ N ∪ {⊥e}` is the active frontier and
`σ : D ↦ Val` is the state (a total map from the declared channels to values;
each channel `c` has a reducer `⊕c`). Execution starts at `⟨route(⊥s), σ0⟩` with
`dom(σ0) = D`.

Running a node `n` yields a write list `⟦n⟧ = [(c1,v1), …, (ck,vk)]` — the
channels it *actually* writes at runtime (not necessarily those it *declared*).
The state-update judgment `σ ⊢ W ⇒ σ'` applies a write list:

                                           c ∈ D
    (Apply-ok)   ────────────────────────────────────────────────
                 σ ⊢ (c,v)::W  ⇒  σ[c ↦ σ(c) ⊕c v] ⊢ W ⇒ σ'

                                          c ∉ D
    (Apply-fault) ───────────────────────────────────────
                  σ ⊢ (c,v)::W  ⇒  ⊥                       (throws)

    (Apply-done)  σ ⊢ []  ⇒  σ

`(Apply-fault)` is exactly `GraphState::write` / `apply_writes` throwing
"Write to unknown channel" when `c ∉ D`. `⊥` is the
faulting result in this model. This is the *only* rule that can fault in
state application, and it is the runtime write-guard.

The super-step relation on configurations:

                 σ ⊢ (⋃_{n∈F} ⟦n⟧) ⇒ σ'        F' = route(F, σ')
    (Step)       ──────────────────────────────────────────────────
                 ⟨F, σ⟩  →  ⟨F', σ'⟩

                 σ ⊢ (⋃_{n∈F} ⟦n⟧) ⇒ ⊥
    (Step-fault) ─────────────────────────────
                 ⟨F, σ⟩  →  ⊥

    (Halt)       ⟨{⊥e}, σ⟩  →  ·          (terminates)

`route(F, σ')` computes the next frontier from the taken edges/routes (and any
`goto`/`Send`). A run is the sequence `⟨F0,σ0⟩ → ⟨F1,σ1⟩ → …`, bounded by
`max_steps`; if it reaches `{⊥e}` it **terminates**, if it reaches `⊥` it
**faults**, and if it runs `max_steps` steps without either it **stalls**.

The abstract semantics also includes two structural dispatch faults:

- **Dangling route.** If `route` must follow an edge/route to a name `m ∉ N ∪
  {⊥e}`, dispatch is undefined (a dangling reference). Model this as `→ ⊥`.
- **Empty-route dispatch.** If a conditional node `n` has route map `R(n) = ∅`,
  the map supplies no dispatch target
  in this model. Model this as `→ ⊥`.

## 4. The gate as a well-formedness judgment

`GraphValidator::validate` returns errors and warnings. **Errors** are exactly
the reject conditions; the gate passes iff there are none. We read the four error
families (E3, E4-write, E8, E10) as the premises of `⊢ G ok`:

    (E3)  ∀ (a,b) ∈ E.  a ∈ N ∪ {⊥s}  ∧  b ∈ N ∪ {⊥e}
          ∀ conditional n, label ℓ.  R(n)(ℓ) ∈ N ∪ {⊥e}
          ∀ barrier n, m ∈ wait_for(n).  m ∈ N
    (E4)  ∀ n ∈ N.  wr(n) ⊆ D                       -- declared writes are declared
    (E8)  ∀ barrier n, m ∈ wait_for(n).  ∃ static edge/route into n from m
    (E10) ∀ conditional n with a closed ConditionSpec of labels Λ.
              dom(R(n)) = Λ   (no dead keys, no uncovered labels)
          ∀ conditional n.  R(n) ≠ ∅

          ────────────────────────────────────────────────────────────
                                  ⊢ G ok

The **warnings** (E5 overwrite race, E6 dead channel, E7 unreachable, E9
unsynchronized fan-in, E11 no-path-to-end, E4-read of undeclared) are
deliberately *not* premises of `⊢ ok`: §7 shows none of them can fault, so
requiring their absence would reject safe graphs (over-rejection). This is a
design choice the catalog states as "soundness over coverage."

One coverage caveat, faithfully carried: the effect family (E4/E5/E6) runs only
when **every** node type declared a contract; a single unknown-contract type
disables it. So `(E4)` is a premise of `⊢ G ok` only under

    (Cov)  every node in N has a declared effect contract ρ(n).

Absent `(Cov)`, the gate cannot see writes at all and makes no write-soundness
claim — and neither does the theorem below.

## 5. The honesty hypothesis

The gate reasons about *declared* effects `wr(n)`; the runtime executes *actual*
writes `⟦n⟧`. The bridge is:

    (H)  ∀ n ∈ N.  channels(⟦n⟧) ⊆ wr(n)

i.e. every node writes only channels it declared. `(H)` is an assumption about
node implementations, not something the static gate can verify — a node's `run`
is opaque C++. `gate_fuzz` Layer 2 exhibits a node that violates `(H)` (declares
`{out}`, writes `phantom`); §6.4 handles that case.

## 6. Soundness

Fix `G` with `⊢ G ok` and `(Cov)`. Write `Reach(G)` for the set of
configurations reachable from `⟨route(⊥s), σ0⟩`.

### 6.1 Lemma (Domain invariant)

*For every `⟨F, σ⟩ ∈ Reach(G)`, `dom(σ) = D`.*

**Proof.** `dom(σ0) = D` by construction. `(Apply-ok)` updates `σ[c ↦ …]` only
for `c ∈ D` (its premise), so it preserves the domain; it never adds or removes a
key. `(Apply-fault)` yields `⊥`, not a state. Hence every non-fault step
preserves `dom(σ) = D`. By induction on run length, the invariant holds
throughout. ∎

### 6.2 Lemma (Writes stay declared)

*Assume `(H)`. For every reachable `⟨F, σ⟩` and every `n ∈ F`,
`channels(⟦n⟧) ⊆ D`.*

**Proof.** `channels(⟦n⟧) ⊆ wr(n)` by `(H)`. `wr(n) ⊆ D` by premise `(E4)` of
`⊢ G ok` (available because `(Cov)` holds). Transitivity gives
`channels(⟦n⟧) ⊆ D`. Taking the join over `n ∈ F`, monotonicity (§2) gives
`channels(⋃_{n∈F} ⟦n⟧) ⊆ D`. ∎

### 6.3 Theorem (Progress / non-fault)

*Assume `⊢ G ok`, `(Cov)`, and `(H)`. No reachable configuration steps to `⊥`.
Every run either terminates (reaches `{⊥e}`) or stalls at `max_steps`; it never
faults.*

**Proof.** A step reaches `⊥` only by `(Step-fault)`, a dangling route, or an
empty-route dispatch. We rule out each.

- **Write fault `(Step-fault)`.** This requires `σ ⊢ (⋃_{n∈F} ⟦n⟧) ⇒ ⊥`, i.e.
  some `(c,v)` in the combined write list with `c ∉ D`. By Lemma 6.2,
  `channels(⋃_{n∈F} ⟦n⟧) ⊆ D`, so no such `c` exists. `(Apply-fault)` is never
  enabled; every application reduces by `(Apply-ok)`/`(Apply-done)` to a state
  `σ'` (with `dom(σ') = D` by Lemma 6.1). Contradiction.

- **Dangling route.** `route` follows an edge `(a,b) ∈ E` or a route target
  `R(n)(ℓ)` or a barrier member. Premise `(E3)` gives `b ∈ N ∪ {⊥e}`,
  `R(n)(ℓ) ∈ N ∪ {⊥e}`, and every barrier member `∈ N`. So every followed target
  names an existing node or `⊥e`; dispatch is defined.

- **Empty-route dispatch.** Requires a conditional `n` reached with `R(n) = ∅`.
  Premise `(E10)` gives `R(n) ≠ ∅` for every conditional node. Excluded.

No rule to `⊥` is enabled at any reachable configuration. Since the step relation
is otherwise total (a super-step always produces a next frontier), every run is
an infinite-or-terminating chain of non-fault steps, truncated at `max_steps`.
Thus it terminates or stalls, never faults. ∎

`(E8)` checks declared static incoming edges/routes for barrier members. It
does not prove that all members will execute: mutually exclusive routes can
prevent an AND-join from firing. This proof does not establish barrier liveness
or distinguish every deadlock from a deliberate cycle. Non-fault within the
abstract model does not require a termination guarantee.

### 6.4 Corollary (Soundness of the gate, relative to honest contracts)

*If the gate passes (`⊢ G ok`) with full contract coverage `(Cov)`, and all
contracts are honest `(H)`, then execution does not fault: no write to an
undeclared channel, no dangling-route or empty-route dispatch fault.*

The modeled implication is `gate-pass ⟹ no-modeled-fault`; its contrapositive is
`modeled-fault ⟹ gate-reject`, not `gate-reject ⟹ fault`. The historical corpus
checks particular rejected examples; it does not prove every rejection must
fault. The retained fuzz result reports no post-pass fault among 2000 mutants.

### 6.5 Proposition (`(H)` is necessary; the runtime is the backstop)

*Without `(H)`, `⊢ G ok` does not imply non-fault — but the composite system
(gate + runtime guard) is still fail-stop: it never reaches undefined behaviour,
only a clean `⊥`.*

**Proof.** *Necessity.* Take `G` = a coherent seed plus a node `n` with
`ρ(n) = ({}, {out})`, `out ∈ D`, but `⟦n⟧ = [(phantom, 1)]`, `phantom ∉ D`.
Every premise of `⊢ G ok` holds (in particular `(E4)`: `wr(n) = {out} ⊆ D`), so
the gate passes. Yet running `n` triggers `(Apply-fault)` on `phantom`, so the
run faults. Hence `⊢ G ok ⇏ non-fault` when `(H)` fails. (This is exactly the
`gate_fuzz` Layer-2 lying node, 500/500.)

*Fail-stop.* The violation is caught by `(Apply-fault)` itself — a defined,
deterministic `throw`, not undefined behaviour. So even without `(H)`, execution
cannot corrupt state or run off into UB: it stops with a diagnostic. The runtime
write-guard is thus the sound backstop for the one hypothesis the static gate
must assume. ∎

The two together give the *unconditional* safety statement, which is the honest
one to make about the whole system:

> **The static gate is sound relative to honest effect contracts (Thm 6.3 /
> Cor 6.4). The runtime write-guard makes execution fail-stop even when a
> contract lies (Prop 6.5). No configuration reachable under `⊢ G ok` exhibits
> undefined behaviour within this abstract machine, under its stated assumptions.**

## 7. Why the warnings are not premises

For completeness (in the "no over-rejection" sense), each warning class is shown
harmless to progress — justifying its exclusion from `⊢ G ok`:

- **E7 (unreachable node), E11 (no path to `__end__`).** An unreachable node is
  never in any reachable frontier `F`, so it contributes no writes and no routing
  — it cannot fault (Thm 6.3 quantifies over `n ∈ F` only). E11 concerns
  termination, not faulting.
- **E9 (unsynchronized fan-in), E5 (overwrite race).** These make the *value* in
  a channel depend on super-step arrival order (nondeterminism), but every write
  still targets `c ∈ D` (Lemma 6.2 is unaffected), so no fault. The reducer
  `⊕c` is total. Order-dependence is a semantic hazard, not a stuck state.
- **E6 (dead channel).** A declared-but-unwritten channel keeps its initial
  value; a read of it yields a value in-domain. No fault.
- **E4-read (read of undeclared channel).** Modeled as yielding `null` (the code
  returns a null value, it does not throw on reads). No fault — hence a warning,
  not an error, unlike E4-write.

Each is a real semantic property but none can drive `→ ⊥`; forbidding them would
reject safe harnesses, so the gate keeps them as warnings. This is the formal
content of the catalog's "soundness over coverage".

## 8. Scope and honest limits

- **Not termination.** Soundness here is *non-fault*, not *terminating*. Cycles
  are legitimate (loops via `goto`/`Send`) and run to `max_steps` → stall. The
  theorem says a gate-passing graph never *throws* or hits *UB*, not that it
  halts. `gate_fuzz` treats stalls as non-faults for exactly this reason.
- **Abstraction fidelity.** The proof is over the model of §3, which mirrors the
  code but is not extracted from it. The empirical harnesses are the fidelity
  check; a divergence between them and the theorem would indicate the model (or
  the code) drifted — `gate_fuzz` Layer 1 is the standing regression guard.
- **Coverage `(Cov)`.** Write-soundness (E4) is claimed only when every node type
  declares a contract, mirroring the "one unknown type disables the family"
  gate. Under partial coverage the gate is silent and so is the theorem.
- **Honesty `(H)`.** Assumed, necessary (Prop 6.5), and backstopped at runtime.
  A mechanized proof of `(H)` for a specific node would require verifying its
  `run` body — out of scope here; the write-guard is the pragmatic substitute.
- **Static routing only.** Premise `(E3)` constrains the *static* edge/route/
  barrier structure, so the dangling-route case of Thm 6.3 covers targets the
  gate can see. A `Command.goto`/`Send` computes its target as a *runtime value*
  (this is exactly why E7 reachability is a warning, not an error). Such dynamic
  targets are outside the static gate's view — the same shape of gap as `(H)`
  for writes — and are the scheduler's runtime concern, not a claim of this
  theorem. The proof scopes to statically-routed dispatch; dynamic routing is
  analogously runtime-checked.

## 9. Relation to the empirical harnesses

| Artifact | What it does with this theory |
|---|---|
| [`the_beast_gate_eval.cpp`](the_beast_gate_eval.cpp) | Tests Cor 6.4 on a labeled corpus (4/4): every `⊢ G ok` graph runs clean, every error graph faults. |
| [`the_beast_gate_fuzz.cpp`](the_beast_gate_fuzz.cpp) | Layer 1 = statistical Cor 6.4 (0 faults after a pass over 2000 mutants). Layer 2 = Prop 6.5 (lying contract passes the gate 500/500, runtime backstops 500/500). |

The proof states the guarantee; the harnesses keep the model honest. Neither is
sufficient alone — which is the whole methodology.
