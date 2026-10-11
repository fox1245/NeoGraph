# Program diagnostics

**Languages:** [English](diagnostics.md) | [한국어](diagnostics.ko.md) | [日本語](diagnostics.ja.md) | [简体中文](diagnostics.zh-CN.md)

A Program compilation returns diagnostics: `ProgramCompileError::diagnostics()` when it fails, `ProgramBundle::compile_diagnostics()` when it succeeds, and `.diagnostics` on the Python exceptions. This page explains each code and how to fix it. Every returned diagnostic links its entry here through `help.doc`.

- [The help object](#the-help-object)
- [Program document](#program-document)
- [JavaScript frontend](#javascript-frontend)
- [Core parser](#core-parser)
- [Core validator](#core-validator)
- [Registry](#registry)
- [Runtime and admission](#runtime-and-admission)

## The help object

Returned diagnostics carry `help`, which tools and models can act on without parsing the message:

| Field | Content |
|---|---|
| `doc` | The link to the code's entry on this page, or to its family's section. |
| `related` | Other locations of the same root cause, each with a one-line `label`. |
| `suggestions` | Fixes. Each has a `message`, `edits` and an `applicability`. |
| `candidates` | Declared names within two edits of an unknown name, nearest first. |

`edits` are RFC 6902 JSON Patch operations (`add`, `replace`, `remove`, `move`) on the Program document the compiler parsed. For a JavaScript source that is the document `define()` generated, and the diagnostic's span locates the builder call. `applicability` is `machine` (apply as given), `maybe` (review first; the default) or `placeholder` (fill in a value such as `<node>`).

The diagnostics sealed into a bundle never carry `help`, so bundle ids do not depend on it. A compilation returns one diagnostic per root cause: when several diagnostics report the same unknown name, the first is kept and the others become its `related` entries. At most 20 diagnostics are returned, errors first; a `P_DIAGNOSTICS_TRUNCATED` note counts the rest.

Messages name what you wrote: builder calls such as `graph.barrier("D", [...])` for a JavaScript source, and Core keys for a JSON or C++ builder document.

## Program document

| Code | Meaning and fix |
|---|---|
| <a id="p_schema_version"></a>`P_SCHEMA_VERSION` | The Program, contract or embedded Core definition uses an unsupported or missing schema version. Use the versions the compiler lists in the witness. |
| <a id="p_schema_unknown_field"></a>`P_SCHEMA_UNKNOWN_FIELD` | A closed Program object has a field it does not define. Remove it or use the candidate. |
| <a id="p_schema_required"></a>`P_SCHEMA_REQUIRED` | A required Program field is missing. Add it. |
| <a id="p_schema_type"></a>`P_SCHEMA_TYPE` | A Program field has the wrong JSON type, or a source map repeats a pointer. Use the type in the witness. |
| <a id="p_schema_operation"></a>`P_SCHEMA_OPERATION` | The operation needs a newer Program schema version. Raise `program_schema_version` or use another operation. |
| <a id="p_contract_schema"></a>`P_CONTRACT_SCHEMA` | An input or output contract uses an invalid or unsupported JSON Schema keyword. |
| <a id="p_root_name"></a>`P_ROOT_NAME` | The root name is missing, empty or differs from the Core definition's `name`. Make them equal. |
| <a id="p_root_operation"></a>`P_ROOT_OPERATION` | A `call_core` root has no Core nodes. Declare at least one node. |
| <a id="p_plan_operation"></a>`P_PLAN_OPERATION` | The operation is not supported. |
| <a id="p_plan_core"></a>`P_PLAN_CORE` | A plan calls a Core other than the sealed root, or calls none and dispatches no child. |
| <a id="p_plan_bound"></a>`P_PLAN_BOUND` | A loop, retry or timeout bound is outside its range. |
| <a id="p_plan_condition"></a>`P_PLAN_CONDITION` | A plan condition does not have exactly one predicate, or its path is not a JSON Pointer. |
| <a id="p_plan_race_arity"></a>`P_PLAN_RACE_ARITY` | A race has other than two branches. |
| <a id="p_plan_quorum"></a>`P_PLAN_QUORUM` | `min_success` is zero or larger than the branch count. |
| <a id="p_plan_spawn_shape"></a>`P_PLAN_SPAWN_SHAPE` | A spawn has an inline body or is not the direct body of an await. |
| <a id="p_plan_cancel_scope"></a>`P_PLAN_CANCEL_SCOPE` | Cancel supports only the `run` scope. |
| <a id="p_plan_reference"></a>`P_PLAN_REFERENCE` | A sealed operation refers to an operation that does not exist. This is a compiler fault; report it. |
| <a id="p_plan_source_pointer"></a>`P_PLAN_SOURCE_POINTER` | A sealed operation has an invalid source pointer. This is a compiler fault; report it. |
| <a id="p_plan_typed"></a>`P_PLAN_TYPED` | The plan could not be sealed as typed operations. The witness has the detail. |
| <a id="p_parallel_map_source"></a>`P_PARALLEL_MAP_SOURCE` | `item_source` needs either a bounded literal array or an input artifact field. |
| <a id="p_parallel_map_binding"></a>`P_PARALLEL_MAP_BINDING` | A `parallel_map` binding or endpoint is missing or is not a JSON Pointer. |
| <a id="p_parallel_map_bound"></a>`P_PARALLEL_MAP_BOUND` | A `parallel_map` bound is out of range, or `max_in_flight` exceeds `max_items`. |
| <a id="p_parallel_map_failure"></a>`P_PARALLEL_MAP_FAILURE` | `failure_policy` must be `fail_fast` or `collect`. |
| <a id="p_expand_source"></a>`P_EXPAND_SOURCE` | `proposal_source` needs an inline object or an input JSON Pointer. |
| <a id="p_expand_bound"></a>`P_EXPAND_BOUND` | An `expand_task_graph` bound is out of range. |
| <a id="p_expand_failure"></a>`P_EXPAND_FAILURE` | `failure_policy` must be `fail_fast` or `collect`. |
| <a id="p_budget_invalid"></a>`P_BUDGET_INVALID` | The budget records are incomplete, repeated, inverted or below a structural floor. Declare all nine resources once. |
| <a id="p_budget_v1"></a>`P_BUDGET_V1` | Program schema 1 fixes `max_program_operations` to exactly one. |
| <a id="p_budget_static"></a>`P_BUDGET_STATIC` | The declared maximum cannot cover the operations the plan always runs. Raise it. |
| <a id="p_migration_program_json"></a>`P_MIGRATION_PROGRAM_JSON` | Program JSON operation trees are no longer accepted. Author in JavaScript or a trusted C++ builder. |
| <a id="p_compiler_internal"></a>`P_COMPILER_INTERNAL` | The compiler failed closed on an internal error. Report it with the source. |
| <a id="p_diagnostics_truncated"></a>`P_DIAGNOSTICS_TRUNCATED` | More than 20 diagnostics were found; `witness.omitted` counts the rest. Fix the ones shown and compile again. |

## JavaScript frontend

| Code | Meaning and fix |
|---|---|
| <a id="p_js_evaluation"></a>`P_JS_EVALUATION` | The source threw or did not parse. The span points at the failing line. |
| <a id="p_js_timeout"></a>`P_JS_TIMEOUT` | Evaluation exceeded the compile time limit or the budget's `wall_time_ms`. |
| <a id="p_js_resource_limit"></a>`P_JS_RESOURCE_LIMIT` | Evaluation exceeded the memory or stack limit. |
| <a id="p_js_cancelled"></a>`P_JS_CANCELLED` | Evaluation was cancelled. |
| <a id="p_js_runtime"></a>`P_JS_RUNTIME` | The QuickJS runtime or a builder call failed internally. |
| <a id="p_js_runtime_identity"></a>`P_JS_RUNTIME_IDENTITY` | The source was created for a different QuickJS build or profile than the compiler's. |
| <a id="p_js_unavailable"></a>`P_JS_UNAVAILABLE` | This build has no QuickJS frontend. |
| <a id="p_js_ambient_denied"></a>`P_JS_AMBIENT_DENIED` | The source used an ambient authority (clock, randomness, eval and similar) that the profile denies. |
| <a id="p_js_define_missing"></a>`P_JS_DEFINE_MISSING` | The module does not export `define()`. |
| <a id="p_js_define_async"></a>`P_JS_DEFINE_ASYNC` | `define()` must be synchronous. |
| <a id="p_js_define_value"></a>`P_JS_DEFINE_VALUE` | `define()` must return exactly one open graph builder. |
| <a id="p_js_graph_argument"></a>`P_JS_GRAPH_ARGUMENT` | A builder method received an argument of the wrong shape, a reserved or repeated name, or an undeclared node. The message names the method. |
| <a id="p_js_graph_value"></a>`P_JS_GRAPH_VALUE` | A builder argument is not canonical JSON data (functions, class instances, non-finite numbers). |
| <a id="p_js_graph_limit"></a>`P_JS_GRAPH_LIMIT` | The generated graph exceeds its byte limit. |
| <a id="p_js_graph_owner"></a>`P_JS_GRAPH_OWNER` | A builder method was called on an object that is not this definition's builder. |
| <a id="p_js_graph_sealed"></a>`P_JS_GRAPH_SEALED` | A builder method was called after `define()` returned. |
| <a id="p_js_graph_entry"></a>`P_JS_GRAPH_ENTRY` | The graph has nodes but no `.entry(node)`, so nothing would run. |
| <a id="p_js_control_character"></a>`P_JS_CONTROL_CHARACTER` | A config string contains a control character, often a regular expression written as a plain string. Use `/.../.source` or ``String.raw`...` ``. |
| <a id="p_js_control_main"></a>`P_JS_CONTROL_MAIN` | An exported `main` is not a function. |
| <a id="p_js_control_async"></a>`P_JS_CONTROL_ASYNC` | `main` must be a synchronous generator. |
| <a id="p_js_control_value"></a>`P_JS_CONTROL_VALUE` | `main` yielded or returned a value that is not a sealed command or canonical JSON. |
| <a id="p_js_control_command"></a>`P_JS_CONTROL_COMMAND` | A command was built with invalid arguments. |
| <a id="p_js_control_finished"></a>`P_JS_CONTROL_FINISHED` | The control generator was resumed after it finished. |
| <a id="p_js_import_dynamic"></a>`P_JS_IMPORT_DYNAMIC` | Dynamic `import()` and `require()` are not allowed. |
| <a id="p_js_import_invalid"></a>`P_JS_IMPORT_INVALID` | An import specifier is malformed. |
| <a id="p_js_import_unavailable"></a>`P_JS_IMPORT_UNAVAILABLE` | An import is not part of the sealed module closure. |
| <a id="p_import_conflict"></a>`P_IMPORT_CONFLICT` | One import source appears with two content identities. |
| <a id="p_import_dynamic"></a>`P_IMPORT_DYNAMIC` | The source or a sealed module loads a module dynamically. |
| <a id="p_import_unresolved"></a>`P_IMPORT_UNRESOLVED` | An import is not bound to an exact verified module receipt. Compile with the module resolution. |

## Core parser

These come from the Core definition parser; the witness carries the Core code as `core_code`.

| Code | Meaning and fix |
|---|---|
| <a id="p_core_gc_schema_version"></a>`P_CORE_GC_SCHEMA_VERSION` | The Core `schema_version` is not a supported non-negative integer. |
| <a id="p_core_gc_unknown_field"></a>`P_CORE_GC_UNKNOWN_FIELD` | A strict Core object has a key the engine does not read. Remove it, use the candidate, or move a note to a `_` or `x-` annotation key. |
| <a id="p_core_gc_field_type"></a>`P_CORE_GC_FIELD_TYPE` | A Core field has the wrong JSON type. |
| <a id="p_core_gc_field_value"></a>`P_CORE_GC_FIELD_VALUE` | A Core field has an invalid value: an empty name, an out-of-range number or an unknown option. |
| <a id="p_core_gc_field_required"></a>`P_CORE_GC_FIELD_REQUIRED` | A required Core field such as `type`, `from` or `to` is missing. |
| <a id="p_core_gc_edge"></a>`P_CORE_GC_EDGE` | An edge or conditional edge is not an object, or its routes are not an object. |
| <a id="p_core_gc_barrier"></a>`P_CORE_GC_BARRIER` | A barrier's `wait_for` is missing, empty or malformed. |
| <a id="p_core_gc_node_config"></a>`P_CORE_GC_NODE_CONFIG` | A node's config does not match the `required`, `type` or `enum` rules of its type's schema. |
| <a id="p_core_gc_reserved_node"></a>`P_CORE_GC_RESERVED_NODE` | `__start__` and `__end__` are reserved and cannot name nodes. |
| <a id="p_core_gc_no_entry"></a>`P_CORE_GC_NO_ENTRY` | No edge leaves `__start__`, so nothing would run. Add `.entry(node)`. |
| <a id="p_core_gc_registry_miss"></a>`P_CORE_GC_REGISTRY_MISS` | A node type, reducer or condition is missing from the registry snapshot. Register it or use the candidate. |
| <a id="p_core_gc_metadata_limit"></a>`P_CORE_GC_METADATA_LIMIT` | Graph metadata is over a limit: 8 KiB of canonical JSON, nesting depth 4, 64 object members in total or 128-byte keys. The witness gives `kind`, `limit` and `actual`. |
| <a id="p_core_gc_metadata_text"></a>`P_CORE_GC_METADATA_TEXT` | A metadata key or string contains an invisible character (a C0 control other than tab, newline or carriage return, a bidi control, a zero-width character or a tag character) or ill-formed UTF-8. The witness lists the code points. |
| <a id="p_core_roundtrip"></a>`P_CORE_ROUNDTRIP` | The parsed graph does not re-emit the definition: something was dropped or rewired. This is a compiler fault; report it. |

## Core validator

These analyze what the graph means. Warnings are returned only; a bundle seals the warnings of the rules it was first sealed with.

| Code | Meaning and fix |
|---|---|
| <a id="p_core_e3"></a>`P_CORE_E3` | An edge, route, interrupt or barrier names an unknown node, or a barrier waits for itself. Use the candidate. |
| <a id="p_core_e4"></a>`P_CORE_E4` | A node's effect contract writes (error) or reads (warning) an undeclared channel. Declare the channel. |
| <a id="p_core_e5"></a>`P_CORE_E5` | Fan-out siblings write the same overwrite channel; the last writer wins nondeterministically. Use an append reducer. |
| <a id="p_core_e6"></a>`P_CORE_E6` | A channel is never read or written, written but never read, or read but never written and has no initial value. |
| <a id="p_core_e7"></a>`P_CORE_E7` | Nodes cannot be reached from `__start__` through edges and routes. |
| <a id="p_core_e8"></a>`P_CORE_E8` | A barrier waits for a node that never routes to it, so the join can never fire. |
| <a id="p_core_e9"></a>`P_CORE_E9` | A node has several plain in-edges and no barrier; it runs once per arriving super-step. Add a barrier for an AND join, or ignore it for an XOR merge. |
| <a id="p_core_e10"></a>`P_CORE_E10` | A conditional edge has no routes, routes a label its condition never returns, or leaves a label unrouted. |
| <a id="p_core_e11"></a>`P_CORE_E11` | Nodes are trapped in a cycle with no path to `__end__`. |
| <a id="p_core_e12"></a>`P_CORE_E12` | The local registry lacks a reducer, node type or condition. |
| <a id="p_core_e13"></a>`P_CORE_E13` | A static edge leaves a node that routes through a conditional edge; it never fires. Remove it or add it as a route. |
| <a id="p_core_e14"></a>`P_CORE_E14` | A node has a second conditional edge; only the first one routes. Merge them. |
| <a id="p_core_e15"></a>`P_CORE_E15` | A barrier member can route somewhere other than its join; when it does, the run ends barrier-stalled. |
| <a id="p_core_e16"></a>`P_CORE_E16` | The graph has a cycle but this compile requires an acyclic graph. |

## Registry

| Code | Meaning and fix |
|---|---|
| <a id="p_registry_missing"></a>`P_REGISTRY_MISSING` | A referenced executable is absent from the registry snapshot. Register it or use the candidate. |
| <a id="p_registry_dependency_missing"></a>`P_REGISTRY_DEPENDENCY_MISSING` | An executable needs a dependency the registry snapshot lacks. |
| <a id="p_registry_identity_mismatch"></a>`P_REGISTRY_IDENTITY_MISMATCH` | Two references resolve to different exact identities of one executable. |
| <a id="p_registry_requirement_kind"></a>`P_REGISTRY_REQUIREMENT_KIND` | A node's requirement resolver returned an unsupported executable kind. |
| <a id="p_registry_requirement_resolver"></a>`P_REGISTRY_REQUIREMENT_RESOLVER` | A node's requirement resolver threw. |

## Runtime and admission

Admission and run failures reach Python as `ng.ProgramAdmissionError` and `ng.ProgramDiagnosticError`, and run results carry a `failure` with a code. Their `help.doc` links this section unless the code has its own entry.

| Code | Meaning and fix |
|---|---|
| <a id="p_core_barrier_stalled"></a>`P_CORE_BARRIER_STALLED` | The Core run stopped with a join still waiting for signals that can no longer arrive. The witness lists each stalled join with `received` and `missing`. |
