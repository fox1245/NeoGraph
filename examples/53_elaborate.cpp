// NeoGraph Example 53: DSL → core topology elaboration (issue #75 M4)
//
// The lockfile workflow: author a harness in the DSL surface (vars /
// templates / use — see include/neograph/graph/elaborator.h), then
// expand it to the frozen core grammar the engine consumes:
//
//   ./example_elaborate harness.dsl.json > harness.json
//   ./example_elaborate harness.dsl.json --map harness.map.json
//
// The expanded core is the artifact you commit and diff-review (the
// "lockfile"); the source map ties every generated construct back to
// the DSL coordinate that produced it. Elaboration is total and
// deterministic — the same DSL source always yields byte-identical
// core JSON — and errors are reported against DSL coordinates
// ("use[2].args: missing arg ..."), never against the expanded output.
//
// The output is strict by construction if the source declares
// "schema_version": 1 — feed it to GraphEngine::compile and the M1/M2
// gates (consumed-key accounting, translation validation, static
// semantics) do the rest.

#include <neograph/graph/elaborator.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0]
                  << " <dsl.json> [--map <sourcemap.json>]\n";
        return 2;
    }

    std::ifstream in(argv[1]);
    if (!in.is_open()) {
        std::cerr << "cannot open " << argv[1] << "\n";
        return 2;
    }
    std::stringstream buf;
    buf << in.rdbuf();

    try {
        const auto doc = neograph::json::parse(buf.str());
        const auto result = neograph::graph::Elaborator::elaborate(doc);
        std::cout << result.core.dump(2) << "\n";

        for (int i = 2; i + 1 < argc; ++i) {
            if (std::string(argv[i]) == "--map") {
                std::ofstream mf(argv[i + 1]);
                mf << result.sourcemap.dump(2) << "\n";
            }
        }
    } catch (const std::exception& e) {
        std::cerr << e.what() << "\n";
        return 1;
    }
    return 0;
}
