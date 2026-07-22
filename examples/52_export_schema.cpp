// NeoGraph Example 52: Export the topology JSON Schema
//
// NeoGraph runs a graph that is *described in JSON* — swap the JSON and
// the same engine becomes a different harness. That makes a code-free
// visual editor possible (see issue #56: a block-coding topology
// editor in a separate repo). For the editor's palette to never drift
// from the engine, the engine itself emits a machine-readable schema:
//
//   { "neograph_version": "...",
//     "$schema": "https://json-schema.org/draft/2020-12/schema",
//     "topology":   { ...JSON Schema for the top-level envelope... },
//     "node_types": { "<type>": { ...config schema... }, ... },
//     "reducers":   [ ... ],
//     "conditions": [ ... ] }
//
// `node_types` reflects whatever is registered in NodeFactory at call
// time, so an embedder's custom node types appear here too — register
// them before calling, exactly as you would before build().
//
// Usage:
//   ./example_export_schema > schema.json
//
// A separate editor repo's CI runs this against the pinned NeoGraph
// version and ships the resulting schema.json as the palette source —
// the editor and the engine cannot fall out of sync.

#include <neograph/graph/loader.h>

#include <iostream>

int main() {
    // (Register any custom node types / reducers / conditions here
    //  first if you want them in the exported palette, e.g.:
    //    NodeFactory::instance().register_type("my_node", factory, schema);)

    std::cout << neograph::graph::NodeFactory::instance()
                     .export_schema()
                     .dump(2)
              << "\n";
    return 0;
}
