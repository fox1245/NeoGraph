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

#include <neograph/program/admission.h>
#include <neograph/program/bundle.h>
#include <neograph/program/compiler.h>
#include <neograph/program/diagnostic.h>
#include <neograph/program/registry.h>
#include <neograph/program/source.h>
#include <neograph/program/version.h>
