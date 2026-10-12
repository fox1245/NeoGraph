#include "graph_host_contract.h"
#include <neograph/a2a/client.h>
#include <neograph/a2a/server.h>

#include <condition_variable>
#include <map>
#include <mutex>

namespace {
using namespace graph_host_contract;
namespace a2a = neograph::a2a;

class A2AHost : public Host {
public:
    A2AHost(GraphFixture& fixture, std::size_t limit)
        : server_(fixture.engine, card()) {
        a2a::test::A2AServerTestAccess::set_max_inflight_runs(server_, limit);
        if (!server_.start_async("127.0.0.1", 0))
            throw std::runtime_error("A2A contract server failed to listen");
        url_ = "http://127.0.0.1:" + std::to_string(server_.port());
    }
    ~A2AHost() override { shutdown(); }

    std::future<Result> start(const std::string& identity,
                               const std::string& prompt, int) override {
        return std::async(std::launch::async, [this, url = url_, identity, prompt] {
            // A2A task ids are server-generated (spec 3.4.2): the contract
            // identity maps to the id the opening Task of the stream names.
            a2a::MessageSendParams request;
            request.message.message_id = "contract-request-" + identity;
            request.message.role = a2a::Role::User;
            request.message.parts.push_back(a2a::Part::text_part(prompt));
            request.message.context_id = "contract-context";
            Result result;
            a2a::A2AClient client(url);
            client.set_timeout(std::chrono::seconds(3));
            a2a::Task task;
            try {
                task = client.send_message_stream(request,
                    [&](const a2a::StreamEvent& event) {
                        if (event.type == a2a::StreamEvent::Type::Task && event.task) {
                            remember(identity, event.task->id);
                        }
                        if (event.type == a2a::StreamEvent::Type::StatusUpdate &&
                            event.status_update) {
                            result.events.push_back(event.status_update->final
                                                    ? "terminal" : "working");
                        }
                        return true;
                    });
            } catch (const a2a::A2ARpcError& error) {
                // Admission refusals are a system error with retry guidance
                // (A2A-22), the A2A form of a rejected request.
                if (error.code() != -32603) throw;
                result.terminal = Terminal::rejected;
                return result;
            }
            switch (task.status.state) {
                case a2a::TaskState::Completed: result.terminal = Terminal::completed; break;
                case a2a::TaskState::InputRequired: result.terminal = Terminal::interrupted; break;
                case a2a::TaskState::Canceled: result.terminal = Terminal::cancelled; break;
                case a2a::TaskState::Rejected: result.terminal = Terminal::rejected; break;
                case a2a::TaskState::Failed:
                    result.terminal = task.metadata.value("neograph/invocation_status",
                                                         std::string()) == "max_steps"
                        ? Terminal::max_steps : Terminal::error;
                    break;
                default: throw std::runtime_error("A2A returned a nonterminal task");
            }
            if (task.status.message && !task.status.message->parts.empty())
                result.output = task.status.message->parts.front().text;
            return result;
        });
    }
    void cancel(const std::string& identity) override {
        a2a::A2AClient client(url_);
        (void)client.cancel_task(task_id(identity));
    }
    std::string effective_identity(const std::string& identity) const override {
        return task_id(identity);
    }
    void shutdown() override { server_.stop(); }
private:
    void remember(const std::string& identity, const std::string& id) {
        std::lock_guard lock(ids_mu_);
        ids_.emplace(identity, id);
        ids_cv_.notify_all();
    }
    std::string task_id(const std::string& identity) const {
        std::unique_lock lock(ids_mu_);
        ids_cv_.wait_for(lock, std::chrono::seconds(2), [&] { return ids_.contains(identity); });
        const auto it = ids_.find(identity);
        return it == ids_.end() ? identity : it->second;
    }

    static a2a::AgentCard card() {
        a2a::AgentCard result;
        result.name = "contract";
        result.description = "in-process graph host contract";
        result.url = "http://127.0.0.1/";
        result.version = "0.0.1";
        result.protocol_version = "0.3.0";
        result.preferred_transport = "JSONRPC";
        result.default_input_modes = {"text/plain"};
        result.default_output_modes = {"text/plain"};
        return result;
    }
    a2a::A2AServer server_;
    std::string url_;
    mutable std::mutex ids_mu_;
    mutable std::condition_variable ids_cv_;
    std::map<std::string, std::string> ids_;
};

TEST(GraphHostA2AContract, SharedBoundary) {
    check_contract([](GraphFixture& fixture, std::size_t limit) {
        return std::make_unique<A2AHost>(fixture, limit);
    });
}

// Mutation witness: intentionally drop only the host's cancellation call.
// The same live in-flight observation must now report normal completion.
class BrokenCancellationHost final : public A2AHost {
public:
    using A2AHost::A2AHost;
    void cancel(const std::string&) override {}
};

TEST(GraphHostA2AContract, MissingCancellationFailsSharedPredicate) {
    const auto evidence = observe_cancellation(
        [](GraphFixture& fixture, std::size_t limit) {
            return std::make_unique<BrokenCancellationHost>(fixture, limit);
        });
    EXPECT_EQ(evidence.rejected, Terminal::rejected);
    EXPECT_EQ(evidence.starts, 1);
    EXPECT_EQ(evidence.running, Terminal::completed);
    EXPECT_EQ(evidence.finished, 1);
    EXPECT_FALSE(cancellation_passes(evidence));
}
} // namespace
