// Q0 control: the same Core-only executable is built with the experimental
// QuickJS option enabled and disabled. It links neograph::core only.

#include <neograph/neograph.h>

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

namespace {

using neograph::json;
using namespace neograph::graph;
using Clock = std::chrono::steady_clock;

class IncrementNode final : public GraphNode {
public:
    explicit IncrementNode(std::string name) : name_(std::move(name)) {}

    asio::awaitable<NodeOutput> run(NodeInput input) override {
        const auto current = input.state.get("counter");
        const auto value   = current.is_number_integer() ? current.get<int>() : 0;
        co_return  NodeOutput{{ChannelWrite{"counter", value + 1}}};
    }

    std::string get_name() const override { return name_; }

private:
    std::string name_;
};

json definition() {
    return {
        {"schema_version", 1},
        {"name", "quickjs-disabled-core-probe"},
        {"channels", {{"counter", {{"reducer", "overwrite"}, {"initial", 0}}}}},
        {"nodes",
         {{"a", {{"type", "quickjs-probe-inc"}}},
          {"b", {{"type", "quickjs-probe-inc"}}},
          {"c", {{"type", "quickjs-probe-inc"}}}}},
        {"edges", json::array({{{"from", "__start__"}, {"to", "a"}},
                               {{"from", "a"}, {"to", "b"}},
                               {{"from", "b"}, {"to", "c"}},
                               {{"from", "c"}, {"to", "__end__"}}})},
        {"conditional_edges", json::array()},
    };
}

std::size_t parse_positive(std::string_view text) {
    std::size_t value       = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("iterations must be a positive integer");
    return value;
}

std::size_t peak_resident_bytes() noexcept {
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0) return 0;
#if defined(__APPLE__)
    return static_cast<std::size_t>(usage.ru_maxrss);
#else
    return static_cast<std::size_t>(usage.ru_maxrss) * 1024u;
#endif
#else
    return 0;
#endif
}

const char* build_type() noexcept {
#ifdef NEOGRAPH_BENCH_BUILD_TYPE
    return NEOGRAPH_BENCH_BUILD_TYPE;
#else
    return "unspecified";
#endif
}

const char* quickjs_build() noexcept {
#if NEOGRAPH_BENCH_QUICKJS_ENABLED
    return "enabled_unused";
#else
    return "disabled";
#endif
}

}  // namespace

int main(int argc, char** argv) {
    try {
        if (argc != 3 || std::string_view(argv[1]) != "--iterations")
            throw std::invalid_argument(
                "usage: bench_core_quickjs_probe --iterations <positive-integer>");
        const auto iterations = parse_positive(argv[2]);

        NodeFactory::instance().register_type(
            "quickjs-probe-inc", [](const std::string& name, const json&, const NodeContext&) {
                return std::make_unique<IncrementNode>(name);
            });
        auto engine = GraphEngine::compile(definition(), NodeContext{});
        (void)engine->run(RunConfig{});

        const auto started = Clock::now();
        for (std::size_t index = 0; index < iterations; ++index)
            (void)engine->run(RunConfig{});
        const auto elapsed =
            std::chrono::duration<double, std::micro>(Clock::now() - started).count();
        const auto value = elapsed / static_cast<double>(iterations);

        std::cout << std::setprecision(17)
                  << "{\"schema_version\":1,\"case\":\"enabled_unused_core\","
                     "\"status\":\"ok\",\"value\":"
                  << value << ",\"unit\":\"us\",\"iterations\":" << iterations
                  << ",\"peak_allocated_bytes\":" << peak_resident_bytes()
                  << ",\"memory_scope\":\"process_peak_resident_set\",\"quickjs_build\":\""
                  << quickjs_build() << "\",\"build_type\":\"" << build_type() << "\"}\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 2;
    }
}
