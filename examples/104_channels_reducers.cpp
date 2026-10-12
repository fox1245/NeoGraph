// NeoGraph Example 104: channels, reducers and versions
//
// A channel is a named slot of graph state. Its reducer decides what a write does to it:
//
//   overwrite   the new value replaces the old one
//   append      the new value (a list) is added to the end of the old list
//
// A channel can start from an `initial` value. Every write also stamps the channel with a
// version, taken from ONE counter shared by all channels: it tells you the order of writes,
// not how many a channel received. A channel nobody wrote keeps version 0.
// Three nodes run in turn, each writing the two channels; the example prints what is left in
// each channel and its version. See example 42 for your own reducer.
//
// No API key required.
//
// Usage: ./example_channels_reducers
//
// smoke-expect-line: overwrite last   = "third"  (version 5)
// smoke-expect-line: append    all    = ["first","second","third"]  (version 6)
// smoke-expect-line: initial   budget = 100  (version 0, never written)
// smoke-forbid: [neograph] lint

#include <neograph/neograph.h>

#include <iomanip>
#include <iostream>

using namespace neograph;
using namespace neograph::graph;

// Writes its own name to `last` (overwrite) and appends it to `all` (append).
class WriterNode : public SyncGraphNode {
public:
    explicit WriterNode(std::string name) : SyncGraphNode(std::move(name)) {}
protected:
    NodeOutput run_sync(NodeInput) override {
        return NodeOutput{{ChannelWrite{"last", json(get_name())},
                           ChannelWrite{"all", json::array({get_name()})}}};   // append needs a list
    }
};

int main() {
    NodeFactory::instance().register_type("writer",
        [](const std::string& name, const json&, const NodeContext&) {
            return std::make_unique<WriterNode>(name);
        });

    const json definition = {
        {"schema_version", TOPOLOGY_SCHEMA_VERSION},
        {"name", "channels_reducers"},
        {"channels", {
            {"last",   {{"reducer", "overwrite"}}},
            {"all",    {{"reducer", "append"}}},
            // `initial` is what a reader sees before the first write.
            {"budget", {{"reducer", "overwrite"}, {"initial", 100}}}
        }},
        {"nodes", {
            {"first",  {{"type", "writer"}}},
            {"second", {{"type", "writer"}}},
            {"third",  {{"type", "writer"}}}
        }},
        {"edges", json::array({
            {{"from", "__start__"}, {"to", "first"}},
            {{"from", "first"}, {"to", "second"}},
            {{"from", "second"}, {"to", "third"}},
            {{"from", "third"}, {"to", "__end__"}}
        })}
    };
    auto engine = GraphEngine::build(definition, EngineConfig{});
    auto result = engine->run(RunConfig{});

    // output["channels"][name] holds {"value": ..., "version": ...}.
    auto show = [&](const char* label, const char* channel, const char* note = "") {
        std::cout << label << " " << std::left << std::setw(6) << channel << " = "
                  << result.channel_raw(channel).dump() << "  (version "
                  << result.output["channels"][channel]["version"].dump() << note << ")\n";
    };
    show("overwrite", "last");
    show("append   ", "all");
    show("initial  ", "budget", ", never written");
    return 0;
}
