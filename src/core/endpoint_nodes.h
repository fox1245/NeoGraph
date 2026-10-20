#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace neograph::graph {
class NodeFactory;
/// The built-in SchemaProvider endpoint node types (#531).
inline constexpr std::array<std::string_view, 11> kEndpointNodeTypes = {
    "image_generate", "image_edit", "embed", "rerank", "moderate", "decide", "speak", "transcribe", "translate",
    "job_submit", "job_collect"};
inline bool is_endpoint_node_type(std::string_view type) {
    return std::find(kEndpointNodeTypes.begin(), kEndpointNodeTypes.end(), type) != kEndpointNodeTypes.end();
}
/// Registers the built-in SchemaProvider endpoint nodes.
void register_endpoint_nodes(NodeFactory& factory);
}  // namespace neograph::graph
