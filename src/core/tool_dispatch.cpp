#include <neograph/tool_dispatch.h>

#include <asio/co_spawn.hpp>
#include <asio/deferred.hpp>
#include <asio/experimental/parallel_group.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>

#include <algorithm>
#include <utility>

namespace neograph {

asio::awaitable<std::vector<ChatMessage>>
dispatch_tool_calls(std::vector<ToolCall> calls, std::vector<Tool*> tools) {
    std::vector<ChatMessage> results;
    if (calls.empty()) co_return results;

    // One worker per call. Resolves the tool, awaits execute_async, and folds
    // every failure mode (unknown tool, bad arguments, throwing tool) into the
    // returned message so a worker never propagates an exception into the
    // parallel group.
    auto worker = [tools](ToolCall tc) -> asio::awaitable<ChatMessage> {
        ChatMessage tool_msg;
        tool_msg.role         = "tool";
        tool_msg.tool_call_id = tc.id;
        tool_msg.tool_name    = tc.name;

        auto it = std::find_if(tools.begin(), tools.end(),
            [&](Tool* t) { return t->get_name() == tc.name; });
        if (it == tools.end()) {
            tool_msg.content = R"({"error": "Tool not found: )" + tc.name + "\"}";
            co_return tool_msg;
        }
        try {
            auto args = json::parse(tc.arguments);
            tool_msg.content = co_await (*it)->execute_async(args);
        } catch (const std::exception& e) {
            tool_msg.content = std::string(R"({"error": ")") + e.what() + "\"}";
        }
        co_return tool_msg;
    };

    // Single call: run inline, skip the parallel-group machinery.
    if (calls.size() == 1) {
        results.push_back(co_await worker(calls.front()));
        co_return results;
    }

    // Multiple calls: fan them out via the same parallel-group idiom the engine
    // uses for independent nodes within a super-step.
    auto ex = co_await asio::this_coro::executor;
    using DeferredOp = decltype(asio::co_spawn(
        ex, worker(std::declval<ToolCall>()), asio::deferred));
    std::vector<DeferredOp> ops;
    ops.reserve(calls.size());
    for (const auto& tc : calls) {
        ops.push_back(asio::co_spawn(ex, worker(tc), asio::deferred));
    }

    auto [order, excs, values] = co_await asio::experimental::make_parallel_group(
        std::move(ops))
        .async_wait(asio::experimental::wait_for_all(), asio::use_awaitable);
    (void)order;  // results are applied in call order, not completion order

    results.reserve(values.size());
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (excs[i]) {
            // Workers catch their own exceptions, so this is a defensive
            // fallback (e.g. bad_alloc) keyed to the originating call.
            ChatMessage m;
            m.role         = "tool";
            m.tool_call_id = calls[i].id;
            m.tool_name    = calls[i].name;
            try {
                std::rethrow_exception(excs[i]);
            } catch (const std::exception& e) {
                m.content = std::string(R"({"error": ")") + e.what() + "\"}";
            } catch (...) {
                m.content = R"({"error": "unknown tool failure"})";
            }
            results.push_back(std::move(m));
        } else {
            results.push_back(std::move(values[i]));
        }
    }
    co_return results;
}

}  // namespace neograph
