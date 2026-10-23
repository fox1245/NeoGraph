#include <neograph/a2a/harness_backend.h>

#include <neograph/a2a/client.h>
#include <neograph/graph/cancel.h>

#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace neograph::a2a {

namespace {

/// Cancels `token` once `limit` has passed, unless the call it bounds is over
/// first. A zero limit bounds nothing.
class CallDeadline {
  public:
    CallDeadline(std::shared_ptr<graph::CancelToken> token, std::chrono::seconds limit)
        : token_(std::move(token)) {
        if (limit.count() <= 0) return;
        timer_ = std::thread([this, limit] {
            std::unique_lock lock(mutex_);
            if (cv_.wait_for(lock, limit, [this] { return finished_; })) return;
            expired_ = true;
            lock.unlock();
            token_->cancel();
        });
    }
    CallDeadline(const CallDeadline&) = delete;
    CallDeadline& operator=(const CallDeadline&) = delete;
    ~CallDeadline() {
        {
            std::lock_guard lock(mutex_);
            finished_ = true;
        }
        cv_.notify_all();
        if (timer_.joinable()) timer_.join();
    }

    bool expired() const {
        std::lock_guard lock(mutex_);
        return expired_;
    }

  private:
    std::shared_ptr<graph::CancelToken> token_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    bool finished_ = false;
    bool expired_ = false;
    std::thread timer_;
};

}  // namespace

mcp::HarnessCapabilityExecutor make_harness_capability_executor(
    std::map<std::string, std::shared_ptr<A2AClient>> agents,
    std::chrono::seconds call_timeout) {
    return [agents = std::move(agents), call_timeout](
               const json& tool, const json& arguments,
               const std::shared_ptr<graph::CancelToken>& cancel) {
        cancel->throw_if_cancelled("before downstream A2A call");
        const auto agent = tool["executor"]["agent"].get<std::string>();
        auto it = agents.find(agent);
        if (it == agents.end() || !it->second) {
            throw std::runtime_error("unresolved A2A agent: " + agent);
        }
        // The job's token ends the call, and so does the deadline, without
        // touching the job's token or the shared client's settings: the call
        // runs under a child of the token that the deadline cancels.
        auto call_cancel = cancel->fork();
        CallDeadline deadline(call_cancel, call_timeout);
        Task task;
        try {
            task = it->second->send_message_sync(arguments.dump(), "", "", call_cancel.get());
        } catch (const graph::CancelledException&) {
            if (!cancel->is_cancelled() && deadline.expired()) {
                throw std::runtime_error(
                    "A2A agent '" + agent + "' did not answer within "
                    + std::to_string(call_timeout.count()) + " s");
            }
            throw;
        }
        cancel->throw_if_cancelled("after downstream A2A call");
        json result;
        to_json(result, task);
        return result;
    };
}

} // namespace neograph::a2a
