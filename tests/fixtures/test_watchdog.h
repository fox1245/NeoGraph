#pragma once

// A bound for tests that talk to a subprocess. Such a test can hang where
// nothing inside the process can time it out: a join in a destructor that
// waits for a pipe read the platform did not cancel, or a server that never
// answers. CTest then reports only "Timeout" after its whole limit (600 s on
// the CI lanes) and shows no output of the test. A watchdog ends the process
// after a short bound and names the test, so a regression shows within
// seconds and in the log.
//
// watch_tests() applies the bound to the tests it names, each either a whole
// suite ("MCPStdioAsync") or one test ("Suite.Name"); it is called at
// namespace scope, once per translation unit that has such tests:
//
//     [[maybe_unused]] const bool kWatched =
//         neograph::graph::test_support::watch_tests({"MCPStdioAsync"});

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace neograph::graph::test_support {

class TestWatchdog final : public ::testing::EmptyTestEventListener {
public:
    TestWatchdog(std::vector<std::string> names, std::chrono::seconds limit)
        : names_(std::move(names)), limit_(limit), thread_([this] { run(); }) {}
    ~TestWatchdog() override {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        changed_.notify_all();
        thread_.join();
    }

    void OnTestStart(const ::testing::TestInfo& info) override {
        const std::string full = std::string(info.test_suite_name()) + "." + info.name();
        if (!watched(info.test_suite_name(), full)) return;
        {
            std::lock_guard lock(mutex_);
            name_ = full;
            deadline_ = std::chrono::steady_clock::now() + limit_;
            armed_ = true;
        }
        changed_.notify_all();
    }

    void OnTestEnd(const ::testing::TestInfo&) override {
        {
            std::lock_guard lock(mutex_);
            armed_ = false;
        }
        changed_.notify_all();
    }

private:
    bool watched(const std::string& suite, const std::string& full) const {
        for (const auto& name : names_)
            if (name == suite || name == full) return true;
        return false;
    }

    void run() {
        std::unique_lock lock(mutex_);
        while (!quit_) {
            if (!armed_) {
                changed_.wait(lock);
                continue;
            }
            if (changed_.wait_until(lock, deadline_) == std::cv_status::timeout && armed_ &&
                std::chrono::steady_clock::now() >= deadline_) {
                std::fprintf(stderr,
                             "\n[  HUNG    ] %s did not finish within %lld s; ending the process "
                             "(a blocked shutdown or a server that never answered)\n",
                             name_.c_str(), static_cast<long long>(limit_.count()));
                std::fflush(stdout);
                std::fflush(stderr);
                // Destructors would wait on whatever the test is stuck in.
                std::_Exit(3);
            }
        }
    }

    const std::vector<std::string> names_;
    const std::chrono::seconds limit_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::string name_;
    std::chrono::steady_clock::time_point deadline_;
    bool armed_ = false;
    bool quit_ = false;
    std::thread thread_;
};

// Bound each of the named tests to `limit` (they take well under a few
// seconds; the limit only has to tell "slow" from "stuck").
inline bool watch_tests(std::initializer_list<const char*> tests,
                        std::chrono::seconds limit = std::chrono::seconds(30)) {
    std::vector<std::string> names;
    for (const char* test : tests) names.emplace_back(test);
    // The listener list owns the watchdog and deletes it at exit.
    ::testing::UnitTest::GetInstance()->listeners().Append(
        new TestWatchdog(std::move(names), limit));
    return true;
}

}  // namespace neograph::graph::test_support
