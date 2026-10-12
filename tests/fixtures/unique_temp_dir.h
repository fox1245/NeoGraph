#pragma once

// Temporary paths for tests. CTest runs each case in its own process, and
// cases often start together; a name built from a clock reading can repeat
// across them, and one case's cleanup then deletes the files another case has
// open. Every test path is named here instead: the temporary directory with
// its symlinks resolved (native custody roots must not inherit one, such as
// macOS's /var), the caller's prefix, and 128 random bits seeded per process.
// UniqueTempDir also creates its directory exclusively, so even a repeated
// name could not be shared.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace neograph::graph::test_support {

// `prefix` followed by 32 random hex digits.
inline std::string unique_temp_name(const std::string& prefix) {
    static thread_local std::mt19937_64 engine = [] {
        std::random_device device;
#ifdef _WIN32
        const auto process = static_cast<std::uint32_t>(::_getpid());
#else
        const auto process = static_cast<std::uint32_t>(::getpid());
#endif
        const auto now = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        std::seed_seq seed{device(), device(), device(), device(), process,
                           static_cast<std::uint32_t>(now), static_cast<std::uint32_t>(now >> 32)};
        return std::mt19937_64(seed);
    }();
    std::ostringstream name;
    name << prefix << std::hex << std::setfill('0') << std::setw(16) << engine() << std::setw(16)
         << engine();
    return name.str();
}

// A path under the resolved temporary directory that no other test names,
// ending in `suffix` (".sqlite" for a database file). Nothing is created.
inline std::filesystem::path unique_temp_path(const std::string& prefix,
                                              const std::string& suffix = {}) {
    return std::filesystem::canonical(std::filesystem::temp_directory_path()) /
           (unique_temp_name(prefix) + suffix);
}

// A private directory, created exclusively and removed with its contents.
class UniqueTempDir {
public:
    explicit UniqueTempDir(const std::string& prefix) {
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto            candidate = unique_temp_path(prefix);
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error)) {
                path_ = std::move(candidate);
                return;
            }
            if (error)
                throw std::filesystem::filesystem_error("cannot create a test directory", candidate,
                                                        error);
        }
        throw std::runtime_error("no unused test directory name");
    }
    ~UniqueTempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    UniqueTempDir(const UniqueTempDir&)            = delete;
    UniqueTempDir& operator=(const UniqueTempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::string file(const std::string& name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

}  // namespace neograph::graph::test_support
