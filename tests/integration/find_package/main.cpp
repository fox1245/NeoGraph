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
#include <neograph/async/run_sync.h>

#include <cstdlib>
#include <iostream>

namespace {

class InstalledProvider final : public neograph::CompletionProvider {
  public:
    std::string get_name() const override { return "installed"; }

  protected:
    asio::awaitable<neograph::ChatCompletion>
    do_invoke(neograph::CompletionRequest request) override {
        neograph::ChatCompletion result;
        result.message.role = "assistant";
        result.message.content = request.params().model;
        co_return result;
    }
};

} // namespace

int main() {
    using namespace neograph;
    using namespace neograph::graph;

    InstalledProvider provider;
    CompletionParams params;
    params.model = "installed-provider";
    const auto completion = neograph::async::run_sync(provider.invoke_request(
        CompletionRequest::collect(std::move(params))));
    if (completion.message.content != "installed-provider") {
        std::cerr << "installed CompletionProvider dispatch failed\n";
        return EXIT_FAILURE;
    }

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
