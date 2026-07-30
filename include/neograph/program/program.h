/**
 * @file program/program.h
 * @brief Public entry point for immutable NeoGraph Program values.
 *
 * Program defines transport-neutral diagnostics, deeply owned source values,
 * and immutable content-addressed bundle/version artifacts. It does not expose
 * compiler, registry, runtime, store, or admission APIs.
 */
#pragma once

#include <neograph/program/bundle.h>
#include <neograph/program/diagnostic.h>
#include <neograph/program/source.h>
#include <neograph/program/version.h>
