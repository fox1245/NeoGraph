#pragma once

namespace neograph::graph {
class NodeFactory;
/// Registers the built-in SchemaProvider endpoint nodes (#531): image_generate, image_edit, decide, embed,
/// rerank, speak, transcribe, translate, moderate, job_submit and job_collect.
void register_endpoint_nodes(NodeFactory& factory);
}  // namespace neograph::graph
