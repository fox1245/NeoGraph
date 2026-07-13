// The smallest program that proves an installed NeoGraph is actually usable:
// include a public header, construct engine types, run a graph, print the result.
//
// It deliberately touches a header that pulls in the vendored asio
// (graph/engine.h ships asio::awaitable through the coroutine surface) and one
// that pulls in the vendored yyjson (json.h). Those are the dependencies that
// leak through NeoGraph's public API, so a consumer cannot compile unless the
// install ships them and the exported targets point at them. Linking alone is
// not enough — this has to *compile*.

#include <neograph/neograph.h>

#include <cstdlib>
#include <iostream>

int main() {
    using namespace neograph;
    using namespace neograph::graph;

    GraphState state;
    state.init_channel("greeting", ReducerType::OVERWRITE,
                       ReducerRegistry::instance().get("overwrite"), json(""));
    state.write("greeting", json("hello from an installed NeoGraph"));

    const auto value = state.get("greeting").get<std::string>();
    std::cout << value << "\n";

    if (value != "hello from an installed NeoGraph") {
        std::cerr << "unexpected channel value\n";
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
