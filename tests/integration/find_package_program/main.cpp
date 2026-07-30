#include <neograph/program/program.h>

#include <cstdlib>
#include <iostream>

int main() {
    using namespace neograph::graph;

    GraphState state;
    state.init_channel("component", ReducerType::OVERWRITE,
                       ReducerRegistry::instance().get("overwrite"),
                       neograph::json(""));
    state.write("component", neograph::json("installed Program component"));

    const auto value = state.get("component").get<std::string>();
    std::cout << value << "\n";
    return value == "installed Program component" ? EXIT_SUCCESS : EXIT_FAILURE;
}
