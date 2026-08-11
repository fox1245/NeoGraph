# QuickJS Migration Completion Runbook

**Status:** Proposed release-completion procedure. It is not evidence that any
remaining gate has passed.

**Date:** 2026-08-11

**Authority:** [QuickJS Control Architecture](QUICKJS_CONTROL_ARCHITECTURE.md),
[QuickJS Control Runtime Migration Plan](QUICKJS_CONTROL_MIGRATION.md), and
[`spec/quickjs-control-runtime.sdd.yaml`](../spec/quickjs-control-runtime.sdd.yaml).

**Executable companion:**
[`spec/quickjs-control-migration-completion.sdd.yaml`](../spec/quickjs-control-migration-completion.sdd.yaml).

---

## 1. Purpose and fixed scope

This runbook turns the remaining Q0 and Q7 release gates into an ordered,
fail-closed procedure. It does **not** change the architecture:

- JavaScript remains the only user-authored graph/control language;
- `GraphEngine` remains the only Core/application-node executor;
- `ProgramRuntime` remains the sole owner of command admission, durable effects,
  replay, cancellation, and child scheduling;
- strict Core JSON remains canonical data, not a public source language; and
- direct C++ construction remains a trusted embedding API, not a compatibility
  excuse for a public Program JSON operation language.

This procedure does not start issue #35 (`trusted_direct`) or a durable Promise
scheduler. Neither is a prerequisite for completing the restricted durable
profile, and neither may be bundled with legacy removal.

## 2. Verified starting point

| Area | Current state | Evidence |
| --- | --- | --- |
| Q1–Q6 base runtime and authoring cutover | Implemented | `quickjs-control-runtime.sdd.yaml` `completion_state`; JavaScript `define()`/generator behavior is covered by `tests/test_harness_program_cutover.cpp`. |
| Core DSL/elaborator and Harness DSL authoring | Removed/rejected | `authoring_cutover_contract.completed_removals` in the parent spec; the Harness translator rejects legacy modes. |
| Legacy storage drain | Passed only by the scoped no-deployment proof | `docs/QUICKJS_CONTROL_MIGRATION.md` records proof `sha256:06f362…fd6217`; it is not a substitute for a later production snapshot. |
| Linux QuickJS | Implemented and exercised | Linux CI enables `NEOGRAPH_BUILD_QUICKJS_CONTROL`; `neograph_quickjs_tests`, ASan/UBSan, and the preregistered performance matrix exist. |
| macOS QuickJS | Partial | The installed shared Program consumer enables QuickJS, but the ordinary macOS build/test job does not enable it and no static QuickJS consumer row exists. |
| Windows QuickJS | Not qualified | `CMakeLists.txt` deliberately rejects `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` on Windows; Windows Program consumers run with QuickJS disabled. |
| Q7 source/runtime removal | Not started | `SourceKind::CanonicalJson`, Program-document schemas, `ProgramPlan`, legacy compiler parsing, and the operation-tree dispatcher remain in source. |

The release is therefore **not** ready for Q7 deletion. Platform qualification
must close first; the final drain proof must be freshly established at the
actual removal boundary.

## 3. Rules that apply to every step

1. **No silent scope reduction.** Windows is a Q0 requirement. A local
   `WIN32` exception is not completion. Removing that requirement needs a
   separately accepted architecture change, not an implementation-side waiver.
2. **No vendor-source drift by accident.** Keep the QuickJS release,
   archive digest, license, excluded `quickjs-libc.c`, and private symbol
   namespace explicit. A platform shim or patch changes the executable
   semantics and must enter the runtime identity and provenance evidence.
3. **No legacy fallback after the cutover fence.** Failed JavaScript admission
   must not select JSON, Core DSL, or the old operation-tree compiler.
4. **No filename-based deletion.** Some retained Program types are canonical
   storage or trusted C++ infrastructure. Classify each dependency as
   legacy-only, shared-with-JavaScript, or shared-with-trusted-C++ before
   deleting it.
5. **No source checkout proof.** The final storage proof comes from a frozen
   complete inventory or the exact no-deployment attestation, never from an
   empty checkout or synthetic CTest fixture.
6. **Every phase is fail-closed.** A failed entry/exit condition stops the
   sequence; it does not downgrade a hard gate to a warning or reopen legacy
   authoring.

## 4. Ordered completion procedure

### M0 — Freeze the release boundary and write the evidence index

**Entry:** the accepted parent contract and this companion spec are present.

1. Select one candidate commit and cutover ID.
2. Record an immutable evidence index outside the source tree. It must bind the
   commit, compiler/toolchain, CMake cache/options, target platform/architecture,
   QuickJS release/archive digest/build options, every command below, and the
   SHA-256 identity of every result file.
3. Record the supported matrix before measurements: Linux x86_64 and arm64,
   macOS, and Windows x64; each must cover Core-only and QuickJS-enabled Program
   where the parent contract requires it.
4. Preserve the existing no-deployment proof as historical evidence only. Do
   not use its 2026-08-10 identity to certify a later deletion release.

**Exit:** the candidate and evidence schema are immutable, and no next phase is
allowed to report a passing result without an evidence-index entry.

### M1 — Complete the QuickJS portability implementation

**Entry:** M0 passed.

1. Make `NEOGRAPH_BUILD_QUICKJS_CONTROL=ON` build on Windows without enabling
   QuickJS by default or adding `quickjs-libc.c`, `std`, `os`, a dynamic module
   loader, or public QuickJS exports.
2. Put any timing, stack-limit, allocator, or compiler compatibility adaptation
   in a reviewed NeoGraph-owned layer. Do not silently patch the vendored archive.
3. Extend `JavaScriptRuntimeIdentity`/provenance so an admitted bundle identifies
   the exact port/shim implementation as well as the upstream archive and build
   options. A replay must fail before dispatch when this identity differs.
4. Keep `neograph_quickjs` private and prefix its static C symbols. Verify that
   a shared `neograph::program` does not export them and that a static consumer
   cannot collide with a separately linked QuickJS.
5. Add a minimal C embedding smoke executable. It must create a runtime/context,
   evaluate deterministic source, bind one explicit C function, enforce an
   interrupt and memory/stack ceiling, and prove that `std`/`os` are absent.

**Exit:** all three supported operating systems can configure and build both
QuickJS-enabled and Core-only configurations from a clean directory, with the
new runtime identity recorded.

### M2 — Qualify the build, package, and installed-consumer matrix

**Entry:** M1 passed.

For every row below, use clean build and install directories. A build-tree-only
success is insufficient.

| Platform | Required QuickJS-enabled rows | Required Core-only rows |
| --- | --- | --- |
| Linux x86_64 | Program static + shared; runtime/Harness tests; C smoke; installed consumer | static + shared installed consumer; no QuickJS link/interface/export evidence |
| Linux arm64 | Program static + shared; runtime/Harness tests; C smoke; installed consumer | static + shared installed consumer |
| macOS | Program static + shared; runtime/Harness tests; C smoke; installed consumer | static + shared installed consumer |
| Windows x64 | Program static + shared with MSVC; runtime/Harness tests; C smoke; installed consumer | static + shared installed consumer |

The installed QuickJS consumer must exercise a **successful** `define()` and
`function* main()` publication/run through the installed package, not merely a
syntax rejection. It must also retain the existing independently linked second
QuickJS collision probe. Extend `scripts/test_find_package.sh` (or replace it
with an equivalent platform-aware driver) so Windows validates static symbol
namespacing and shared export hiding with native inspection tools rather than
skipping the check.

**Exit:** all rows run from the installed prefix and record package metadata,
loader/link closure, executable output, and private-symbol inspection results.

### M3 — Qualify isolation, replay, ABI, and teardown safety

**Entry:** M2 passed.

1. Run the QuickJS runtime isolation corpus, JavaScript/Harness end-to-end
   tests, deterministic replay/recovery tests, fault-injection tests, and native
   ABI conformance on every enabled platform.
2. Linux must run ASan+UBSan and TSan with QuickJS enabled. macOS must run its
   supported ASan/UBSan equivalent with QuickJS enabled. Windows must run the
   supported MSVC AddressSanitizer configuration with QuickJS enabled. Do not
   claim an unavailable sanitizer as having run; record the toolchain-specific
   limitation in the evidence index.
3. Include cancellation during a long JavaScript evaluation, interruption at a
   pending `callCore`, allocator teardown, callback completion after cancellation,
   and a competing second QuickJS engine in the test corpus.
4. Verify source/runtime/profile/native-binding mismatch fails before dispatch,
   including after process restart.

**Exit:** all supported sanitizer/runtime rows pass without a suppression that
hides NeoGraph or QuickJS ownership defects; every rejected fixture confirms
zero dispatch.

### M4 — Close performance, startup, allocation, and binary-size gates

**Entry:** M3 passed.

1. Run the existing Linux blocking matrix:

   ```sh
   scripts/build_quickjs_performance_matrix.sh \
     <fresh-build-root> <evidence-root>/quickjs-performance-linux.json
   ```

   Its accepted preregistration remains the threshold authority; do not relax
   a threshold in response to a failing candidate.
2. Before measuring macOS and Windows, add version-controlled preregistrations
   for their startup, allocation, and enabled-but-unused/Core-only measurements.
   Record repeated cold/warm distributions, not a single timing.
3. Produce a machine-readable qualification report for each platform containing
   the installed Core-only and Program binary sizes, dynamic dependency closure,
   runtime creation/startup samples, allocator high-water marks, and exact
   build configuration. Core-only rows must have no QuickJS object, link
   dependency, exported symbol, allocation path, or size increase attributable
   to QuickJS.
4. Treat a failed or noisy metric as a failed gate until rerun under the
   preregistered method. Never average away a regression or compare different
   option sets.

**Exit:** each blocking performance/size/startup/allocation threshold passes and
its signed result identity is present in the evidence index.

### M5 — Re-establish final storage drain and inventory the deletion boundary

**Entry:** M4 passed; legacy publication/resume is behind the announced fence.

1. If no pre-release or production deployment has ever existed, obtain a fresh
   named no-deployment attestation for this cutover ID. Otherwise place legacy
   writes behind a fence and capture a consistent, read-only snapshot of every
   durable Program and Harness store.
2. For SQLite, use a real consistent snapshot and reject live WAL/SHM/journal
   sidecars. For PostgreSQL, capture a regular custom-format `pg_dump`; never
   mount `PGDATA` or audit a live database.
3. Enumerate every store and every discovered legacy artifact. Classify each as
   `translated`, `drain_only`, or `rejected`; `drain_only`, active/recoverable
   runs, unknown outcomes, active legacy activations, and unscanned references
   are hard blockers.
4. Run the actual final proof immediately before deletion:

   ```sh
   python3 scripts/audit_legacy_drain.py \
     --inventory <inventory.json> \
     --root <frozen-export-root> \
     --output <evidence-root>/legacy-drain-proof.json \
     --require-final
   ```

5. Separately produce a source dependency inventory. For every legacy-looking
   type/file/test/schema, label it `legacy_only`, `shared_with_javascript`, or
   `shared_with_trusted_cpp`, name its replacement or retention reason, and
   name its regression test. Do not infer this from names.

**Exit:** the auditor reports `final_drain.passed_is_true`; the proof and the
source dependency inventory are both bound to the same cutover ID; legacy write
fencing remains active through release.

### M6 — Replace shared dependencies, then delete legacy authoring/runtime code

**Entry:** M5 passed.

1. First migrate retained trusted-C++ call sites away from Program-document JSON
   and the `ProgramPlan` operation-tree dispatcher. A trusted API may construct
   canonical data in process, but it must not expose or persist a second
   user-authored control language.
2. Preserve the JavaScript command path while severing legacy dependencies.
   The replacement must keep the same `ProgramRuntime` admission, budget,
   journal/effect, cancellation, and replay invariants. Add its behavioral
   regression tests before removing the old path.
3. Delete, without aliases or parser fallbacks:
   - Program-document v1–v4 schemas and their public source routes;
   - legacy `CanonicalJson` source decoding once no stored artifact requires it;
   - legacy `ProgramPlan` operation-tree parsing, lowering, and dispatch;
   - Harness Program-JSON translation and legacy-only examples/tests/docs; and
   - compatibility build/link dependencies and retained runtime-selection code.
4. Keep only artifacts that the dependency inventory marks as canonical storage
   or required trusted C++ embedding infrastructure. In particular, do not
   delete JavaScript source/bundle/journal formats or the Harness JavaScript
   translator merely because they share the `program` namespace.
5. Keep the drain-audit tool and immutable proof as release evidence if useful,
   but it must not retain a deployable legacy runtime fallback.

**Exit:** no new or stored execution path can parse, compile, dispatch, or
fallback to Program JSON operation trees; the public authoring boundary exposes
only JavaScript and trusted C++ embedding.

### M7 — Final clean-room verification and release record

**Entry:** M6 passed.

1. Repeat M2–M4 from fresh directories after deletion, including installed
   consumers and sanitizer builds. No result from the compatibility checkout
   may be reused.
2. Run graph-equivalence, storage-migration, deterministic replay, crash/fault,
   native ABI, and JavaScript/Harness smoke suites against the final tree.
3. Re-run M5 if any change after the snapshot could have admitted, resumed, or
   altered legacy state. Otherwise preserve the verified proof identity in the
   release evidence index.
4. Update `completion_state.legacy_runtime_removal` only after all evidence is
   accepted. Then update the architecture, migration plan, public boundary,
   changelog, examples, and issue state in the same release change.
5. Publish the evidence index with the release. A rollback selects a prior
   admitted JavaScript release/activation or restores a compatible binary; it
   never reopens Core DSL or Program JSON authoring.

**Exit:** Q0 and Q7 gates are complete: one user-authored language, one
canonical Core IR, one Program durability/effect model, and no legacy
implementation fallback remain.

## 5. Deletion inventory: required distinctions

The following current areas require explicit classification during M5. This is a
starting checklist, not permission to remove all of them blindly.

| Area | Why it cannot be deleted by name alone |
| --- | --- |
| `include/neograph/program/plan.h`, `src/program/plan.cpp`, legacy portions of `compiler.cpp` and `run_attempt.cpp` | They implement the Program JSON operation tree and are removal candidates, but callers must first move to a non-legacy trusted/C++ or JavaScript path. |
| `SourceKind::CanonicalJson`, `ProgramSource::parse`, bundle/catalog/migration compatibility branches | They decode retained legacy state. They may go only after M5 proves no state requires them. |
| `schemas/program-document-v1.schema.json` through `v4` | They are legacy authoring schemas and should disappear with their public routes. |
| `program-source`, `program-bundle`, `program-version`, command-journal, and strict Core formats | These can be canonical storage needed by JavaScript or trusted C++ and require an individual retention decision. |
| `authoring.h`, JavaScript source/command code, Harness translator | They own the final JavaScript boundary and must remain. |
| `tests/integration/find_package_program/main.cpp` | It presently exercises a C++ builder carrying a Program document; migrate it to the final trusted C++ or JavaScript path before deletion. |
| `scripts/audit_legacy_drain.py` | It is evidence tooling, not a runtime fallback. Retain or archive it deliberately. |

## 6. Completion record

A phase is complete only when its evidence has been added to the immutable
index, its exit condition has passed, and the next phase's entry condition is
true. A green unit suite cannot replace a platform, installed-consumer,
performance, or final-drain gate.
