// A census of the threads of the process, to show that the code under test started none (#557).
//
// Comparing the number of threads before and after is not enough: a thread that has been joined can still be listed
// for a moment (the kernel drops its entry after the join returns), so the count taken before a run can be one higher
// than the count taken after it although nothing was started. The census compares thread ids: only a thread that is
// listed now and was not listed at the first census counts as started.
#pragma once

#include <algorithm>
#include <filesystem>
#include <iterator>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace neograph::test {

class ThreadCensus {
public:
    ThreadCensus() : before_(listed()) {}

    // False where the platform cannot list threads; started() is then always empty.
    bool available() const { return !before_.empty(); }

    // The ids of the threads that exist now and were not listed when the census was taken.
    std::vector<std::string> started() const {
        const auto               now = listed();
        std::vector<std::string> fresh;
        std::set_difference(now.begin(), now.end(), before_.begin(), before_.end(), std::back_inserter(fresh));
        return fresh;
    }

private:
    static std::set<std::string> listed() {
        std::set<std::string> ids;
#ifdef __linux__
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator("/proc/self/task", error))
            ids.insert(entry.path().filename().string());
#endif
        return ids;
    }

    std::set<std::string> before_;
};

}  // namespace neograph::test
