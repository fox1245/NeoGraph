/**
 * @file program/program.h
 * @brief Public entry point for immutable NeoGraph Program values.
 *
 * Program defines transport-neutral diagnostics, deeply owned source values,
 * immutable content-addressed artifacts, executable registry snapshots,
 * immutable admission and policy snapshots, and pure Program compilation. It
 * does not expose runtime, store, or mutable process-global registries.
 */
#pragma once

#include <neograph/program/authoring.h>
#include <neograph/graph/node.h>
#include <neograph/program/admission.h>
#include <neograph/program/bundle.h>
#include <neograph/program/catalog.h>
#include <neograph/program/command.h>
#include <neograph/program/command_journal.h>
#include <neograph/program/compiler.h>
#include <neograph/program/diagnostic.h>
#include <neograph/program/event.h>
#include <neograph/program/fork.h>
#include <neograph/program/graph_migration.h>
#include <neograph/program/handle.h>
#include <neograph/program/journal.h>
#include <neograph/program/lineage.h>
#include <neograph/program/invocation.h>
#include <neograph/program/migration.h>
#include <neograph/program/pending.h>
#include <neograph/program/module.h>
#include <neograph/program/native.h>
#include <neograph/program/replay.h>
#include <neograph/program/replacement.h>
#include <neograph/program/registry.h>
#include <neograph/program/run_record.h>
#include <neograph/program/result.h>
#include <neograph/program/runtime.h>
#include <neograph/program/runtime_instruction.h>
#include <neograph/program/source.h>
#include <neograph/program/synthesis.h>
#include <neograph/program/task_graph_proposal.h>
#include <neograph/program/task_graph_fragment.h>
#include <neograph/program/transition_store.h>
#include <neograph/program/sqlite_transition_store.h>
#include <neograph/program/version.h>
