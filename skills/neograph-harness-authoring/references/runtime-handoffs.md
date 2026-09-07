# Runtime child synthesis and replacement

First obtain the host's available services and effective grant. Do not invent
`ng.compile`, `ng.replace` or MCP replacement tools: these are host operations,
not ambient JavaScript authority.

For a child, propose reviewed-template parameters or source as permitted by the
host. The host binds the proposal to the actual owner, parent run/version and
lineage head, then performs compilation, semantic validation, admission,
publication and binding. Only after a successful binding may the Program yield
`ng.await(ng.spawn(binding, input, spawnSite), timeoutMs, awaitSite)`. A source
proposal cannot supply its own trusted grant or enlarge its parent's budgets.

For self-replacement, yield an explicit JSON state value through
`ng.checkpoint(state, sourceSite)`. The host retains the checkpoint lease,
reserves a dynamic compilation, compiles and admits the successor, then calls
`ProgramRuntime::replace` with that lease. The target receives serialized
`input.handoff` and the exact predecessor run ID. It does not inherit the old
JavaScript heap or mutate a published graph.

Native C++ host entry points are `RuntimeConfig::checkpoint_handler`,
`ProgramRuntime::prepare_child_synthesis`, `ProgramSynthesisGateway`,
`ProgramRuntime::reserve_synthesis`, and `ProgramRuntime::replace`. Use the
repository headers for signatures. The host must persist synthesis intent before
reservation and retain its outcome. The reserve operation refreshes the held
lease's journal head while retaining its checkpoint identity.

Keep the logical parent, inherited child bindings, output contract, remaining
nonrenewable resources and authority consistent across generations. Reusing a
child binding requires its exact recorded input. A fresh child requires a fresh
admitted binding and an attenuated budget. Never reset limits on a new turn,
retry, replacement or restart.

Treat stale-head conflicts as a need to reload the active generation. Treat an
uncertain provider/compiler outcome as a reconciliation case, not permission to
repeat an effect. Cancellation follows the logical family. Use the returned
guarantee label; arbitrary generator control must not be described as Strict.
