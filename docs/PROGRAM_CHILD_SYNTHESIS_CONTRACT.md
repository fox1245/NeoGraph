# Child Program synthesis: host authorization contract

**Languages:** [English](PROGRAM_CHILD_SYNTHESIS_CONTRACT.md) | [한국어](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ko.md) | [日本語](PROGRAM_CHILD_SYNTHESIS_CONTRACT.ja.md) | [简体中文](PROGRAM_CHILD_SYNTHESIS_CONTRACT.zh-CN.md)

This page covers the host's authorization boundary. [SQLite/PostgreSQL persistence and recovery](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md) describes the durable runtime path. A Program cannot submit source on its own, and NeoGraph does not generate the source with a model: the host selects a reviewed source.

## Entry and ownership

`ProgramSynthesisGateway::synthesize_child(proposal, grant, parent)` accepts a
proposal only under a separately selected `ProgramChildSynthesisGrant`. The host
loads the parent version, run record, lineage head, and generation into
`ProgramChildSynthesisParent`. These snapshots and the grant must come from the
host's own stores and reviewed template policy, never from a requesting Program.

This boundary admits an exact reviewed template **instantiation**. Its grant
binds `template_identity` and `program_synthesis_source_identity(source)`, which
covers the complete canonical source envelope, including import identities,
sealed module bodies, source coordinates, and runtime/profile identity. The host
renders and reviews the instantiation before issuing this grant. Template
rendering and its parameter schema are not implemented by this API.
The grant also pins the semantic validator and task-contract identities. Child
compilation must use the parent's sealed registry fingerprint; equal source text
cannot select different registered implementations.

A stored grant's hash proves integrity, not who issued it. Parsing a caller's
self-authored grant is not permission to use it. The gateway does not accept a
serialized authorization receipt in place of its host grant and parent snapshots.

## Checks before reservation

The pure `authorize_program_child_synthesis` operation checks:

- owner, parent run, parent ProgramVersion and bundle, parent policy fingerprint,
  lineage identity, active generation, and the exact record/journal/head pairing;
- that the parent snapshot is running and nonterminal;
- exact reviewed source identity, canonical source-envelope bytes, and sealed
  module count;
- requested capabilities/effects against both the host grant and parent policy,
  and imported module identities against the parent policy;
- all nine child budget dimensions against the host ceiling and available parent
  remainder, without borrowing the parent's in-flight reservation;
- room for one compile, one child, and one descendant-depth hop; and
- a child guarantee floor that cannot weaken the parent's execution guarantee.

These checks do not reserve resources. They produce immutable, read-only
`ProgramChildSynthesisAuthorization` evidence. Other child reservations and
capacity must still be checked at the ordinary `ProgramRuntime::start_child`
boundary. Concurrent preflight results are not independent budget grants.

## Reservation and compilation

The child entry requires `ProgramSynthesisGatewayConfig::reserve_child`. Its
signature receives both the proposal and the authorization, including the exact
`source_lineage_head_id` and `parent_remaining` against which to compare-and-swap.
It must atomically debit one compile against that head, or fail without charging
a different generation. It must not reload a newer head and silently reserve
against that instead.
The host also checks execution ownership, cancellation, and the live deadline,
and charges elapsed wall time when settling the reservation. A stored snapshot
is not evidence that its wall-time allowance is still available now.

The returned `ProgramSynthesisReservation` must bind the same proposal, lineage,
source head, and starting budget. The gateway rejects a changed starting budget,
wrong head, insufficient post-reservation budget, or loss of child/depth capacity
before compiler evaluation. Reservation construction independently requires
exactly one compile debit and forbids any budget increase.

Child compilation uses the requested child's budget ceiling, not the parent's
larger remaining budget. Compilation must preserve the source identity, requested
capability/effect closure, and authorized guarantee floor. The mandatory host
semantic validator must match the grant's validator and contract identities and
runs before Catalog admission. The resulting child policy
cannot carry capability/effect permissions beyond the request or module permissions outside
the submitted source's imports. A semantic rejection does not undo the prior
compile debit. A retry needs a valid head-specific reservation; it must not reuse
stale preflight evidence to replenish budget.

A child-only gateway may omit the generic `reserve` callback. Calling its generic
`synthesize()` entry then fails. Conversely, the existing successor gateway does
not acquire child synthesis permission merely because `reserve` is configured.
Neither entry falls back to the other.

## JavaScript budget and guarantee boundaries

Generator Programs can receive a nonzero dynamic-compile ceiling through
host-owned `ProgramBudgetBounds` and Catalog policy. Default compilation still
grants zero dynamic compiles. A caller cannot increase that limit at `start`.
Declaration-only JavaScript and ordinary C++ plans without `expand_task_graph`
retain their zero-dynamic-compile structural rule.

Dynamic compilation adds no JavaScript command or ambient compiler access.
`ng.hostCapability` remains its existing trusted-native interface; synthesis
installs no plugin there and changes no native C ABI.

The current compiler conservatively labels generator control `Unmanaged`.
The restricted generator language profile must not be presented as an automatic
`Strict` execution-guarantee label. This API preserves that classification. A reviewed
declaration-only child can meet a Strict grant when its Core closure is Strict;
a generator child cannot pass that floor merely because its proposal requests it.
Admitting a weaker child requires an explicit compatible host grant and parent
guarantee, rather than silently lowering the floor during compilation.

## Diagnostics

| Code | Boundary |
|---|---|
| `P_CHILD_SYNTHESIS_OWNER` | Owner mismatch |
| `P_CHILD_SYNTHESIS_GENERATION` | Wrong run/version/policy, stale or inconsistent head, inactive generation |
| `P_CHILD_SYNTHESIS_SOURCE` | Unreviewed instantiation or source/module size limit |
| `P_CHILD_SYNTHESIS_REGISTRY` | Compiled child uses a different sealed registry |
| `P_CHILD_SYNTHESIS_SEMANTICS` | Validator or task-contract identity changed |
| `P_CHILD_SYNTHESIS_AUTHORITY` | Request or child admission policy expands authority |
| `P_CHILD_SYNTHESIS_BUDGET` | Invalid execution budget or missing compile/child/depth/host capacity |
| `P_CHILD_SYNTHESIS_RESERVATION` | Reservation does not bind the authorized head and remaining budget |
| `P_CHILD_SYNTHESIS_GUARANTEE` | Grant or compiled child violates the guarantee floor |

`ProgramSynthesisValidationError` continues to carry semantic rejection evidence.
Compile errors retain the existing compiler diagnostics. Stored budget decoders
reject negative, fractional, and out-of-range integers before canonical identity
verification; invalid numbers cannot wrap into an otherwise valid stored ID.

## Durable integration

The standalone `synthesize_child` API ends at admission. The runtime API adds
atomic reservation, durable stage outcomes, a parent-run/generation-scoped
binding, and dispatch through the existing child lifecycle on SQLite and
PostgreSQL. See [the persistence contract](PROGRAM_CHILD_SYNTHESIS_PERSISTENCE.md)
for configuration, recovery behavior, the recovery matrix, and its stated limits.
The authorization value remains evidence, with no public stored-value
constructor that could be mistaken for fresh authorization.
