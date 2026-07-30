/**
 * @file program/program.h
 * @brief Public entry point for immutable NeoGraph Program values.
 *
 * Program defines transport-neutral diagnostics, deeply owned source values,
 * immutable content-addressed artifacts, executable registry snapshots, and
 * immutable admission and policy snapshots. It does not expose runtime, store,
 * compiler orchestration, or mutable process-global registries.
 */
#pragma once

#include <neograph/program/admission.h>
#include <neograph/program/bundle.h>
#include <neograph/program/diagnostic.h>
#include <neograph/program/registry.h>
#include <neograph/program/source.h>
#include <neograph/program/version.h>
