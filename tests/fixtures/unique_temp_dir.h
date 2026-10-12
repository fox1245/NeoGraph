#pragma once

// A private temporary directory for one test. CTest runs each case of a suite
// in its own process, and cases often start together; a name built from a
// clock reading can repeat across them, and one case's cleanup then deletes
// the database another case has open. The directory gets a random name and is
// created exclusively: creation fails when the directory exists, so no two
// processes ever share one.

#include <neograph/graph/checkpoint.h>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>

namespace neograph::graph::test_support {

class UniqueTempDir {
public:
    explicit UniqueTempDir(const std::string& prefix) {
        const auto base = std::filesystem::temp_directory_path();
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto            candidate = base / (prefix + Checkpoint::generate_id());
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
    std::string file(const char* name) const { return (path_ / name).string(); }

private:
    std::filesystem::path path_;
};

}  // namespace neograph::graph::test_support
