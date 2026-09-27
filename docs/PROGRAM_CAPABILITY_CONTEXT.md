# Program node capabilities and mediated effects

`ProgramCatalog` binds the aggregate executable closure once, but a node
factory should see only the capabilities declared by that node's admitted
configuration. `RegistrySnapshotBuilder` therefore narrows the construction
`NodeContext` using the node manifest's direct executable requirements and its
config-specific requirement resolver. A bound Provider or Tool used by another
node is not an implicit capability of this node.

| Registered node | Provider pointer | Tool pointers | Safe metadata |
| --- | --- | --- | --- |
| Arbitrary `Brokered` C++ factory | None | None | Exact `provider_name` and `tool_definitions` |
| Host-reviewed `add_host_brokered_node` | Exact declared Provider | Exact declared Tools | Exact metadata |
| Fixed `add_core_llm_call` | Exact declared Provider | Exact declared Tools for model definitions | Exact metadata |
| Fixed `add_core_tool_dispatch` | None | Exact declared Tools | Exact metadata |
| `TrustedNative` C++ factory | Exact declared Provider | Exact declared Tools | Exact metadata |

The two fixed Core registrations construct NeoGraph's own `LLMCallNode` and
`ToolDispatchNode`; they do not accept a caller-supplied factory. Register them
with exact, immutable requirement resolvers. A model-authored topology can
select an admitted type and node configuration, but cannot make an arbitrary
brokered factory receive a raw Tool pointer. The standard dispatch node still
calls `dispatch_tool_calls`, which consults the run's `ToolGate` and
`ToolExecutionController` before invoking a bound Tool.

`TrustedNative` is a separate host-attested boundary. Program admission already
requires `TrustedEmbedding` mode and a matching authenticated Catalog host
identity for that effect mode. Native C++ code can retain resources captured
outside `NodeContext` or perform its own effects; these rules are capability
attenuation, not a process sandbox or proof that arbitrary native code is pure.
Hosts must review and pin native factories and declare their effects honestly.

Program Core operations require a host-owned `ProgramCoreToolGrant` for
mediated Tool dispatch. The runtime checks owner, Program version, run,
operation, attempt, **admitted executable-binding fingerprint**, nonempty grant
ID, gate and controller. Missing or stale grants deny the Tool call, including
after reconnect. A denied Tool call is a Tool result and may leave the Program
itself `Completed`; inspect effect receipts when deciding whether the requested
work succeeded.

Hosts that need a persistent grant identity can admit an exact
`ProgramCoreToolGrantRecord` in `SQLiteProgramCoreToolGrantStore` before
starting a run with an explicit run ID. The binding fingerprint is
`capability_binding_receipt_root(version.core_materialization_receipt().capability_bindings)`.
Install `make_durable_core_tool_grant_resolver(store, policy_factory)` as
`RuntimeConfig.core_tool_grant_resolver`: it reloads the record for every
operation, including reconnect, and accepts only a fresh host policy with
identical grant ID, owner, version, run, operation, attempt and binding.
Admission is idempotent only for an identical active record; conflicting IDs
or bindings cannot replace it. A resumed Program advances its attempt, so the
host must explicitly admit the still-authorized grant for that attempt before
resuming. The same grant ID may cover those attempts only within the same
owner/version/run/operation/binding; revocation disables all its admitted
attempts. Stored records contain no credentials, Tool pointers or callbacks.

The host alone decides whether to admit and how to rebuild per-Tool gate and
controller policy. The record is authority evidence, **not** an effect result:
the host must still journal each Tool dispatch, completion and uncertain
outcome through the per-call effect broker. This does not promise exactly-once
external execution or constrain arbitrary trusted native code calling a Tool
outside mediated dispatch.

Existing brokered custom factories that used `NodeContext.provider` or
`NodeContext.tools` must move execution to a fixed Core node, use a separately
reviewed host broker, or be admitted as `TrustedNative` under the host's trusted
embedding policy. `add_host_brokered_node` is the explicit native host-broker
registration path used by Harness workers. It does not make a caller-supplied
factory safe: the host must review and pin its effects and keep each operation
inside the admitted provider/tool closure. Ordinary `add_node` retains the
metadata-only boundary. Direct Core graphs outside Program retain their existing
`NodeContext` behavior. See [#291](https://github.com/fox1245/NeoGraph/issues/291),
[#292](https://github.com/fox1245/NeoGraph/issues/292), and
[#293](https://github.com/fox1245/NeoGraph/issues/293) for the remaining
durability and native-code boundaries.
