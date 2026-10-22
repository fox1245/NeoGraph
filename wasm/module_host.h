// What the parts of the JavaScript module share (#535, #557): the loop that every run of this module instance is pumped
// on, the message of the last failed export, and the provider that a graph or a Program binds.
#pragma once

#include <neograph/async/host_loop.h>
#include <neograph/graph/types.h>
#include <neograph/json.h>

#include <string>

namespace neograph::wasm {

/// The loop that pumps every run of this module instance, on an executor of its own: a Client shuts down the executor
/// it runs on when it is destroyed, so the loop cannot share one.
async::HostLoop& loop();

/// The message that `ng_error()` returns until the next call.
void set_error(std::string message);

/// The node context of a provider description (see ng_compile): a SchemaProvider over the host's fetch, the model and the
/// request controls. Throws std::invalid_argument for a description the SDK rejects.
graph::NodeContext node_context_from(const json& provider);

}  // namespace neograph::wasm
