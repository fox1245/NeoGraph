#include <neograph/mcp/harness.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace neograph::mcp {

struct FileHarnessRecordStore::Impl {
    explicit Impl(std::string directory) : root(std::move(directory)) {
        if (root.empty()) throw std::invalid_argument("FileHarnessRecordStore root must not be empty");
        std::filesystem::create_directories(root / "artifacts");
        std::filesystem::create_directories(root / "runs");
    }

    static void validate_id(const std::string& id) {
        if (id.empty() || !std::all_of(id.begin(), id.end(), [](unsigned char c) {
                return std::isalnum(c) || c == '-' || c == '_';
            })) throw std::invalid_argument("invalid Harness record identifier");
    }

    std::filesystem::path path(const char* collection, const std::string& id) const {
        validate_id(id);
        return root / collection / (id + ".json");
    }

    void save(const char* collection, const std::string& id, const json& record) {
        const auto target = path(collection, id);
        std::lock_guard lock(mutex);
        const auto temporary = std::filesystem::path(
            target.string() + ".tmp." +
            std::to_string(next_temp.fetch_add(1, std::memory_order_relaxed)));
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            if (!output) throw std::runtime_error("cannot open temporary Harness record");
            output << record.dump();
            output.flush();
            if (!output) throw std::runtime_error("cannot write temporary Harness record");
        }
#ifdef _WIN32
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto code = GetLastError();
            std::filesystem::remove(temporary);
            throw std::runtime_error("cannot commit Harness record (Windows error " +
                                     std::to_string(code) + ")");
        }
#else
        std::error_code error;
        std::filesystem::rename(temporary, target, error);
        if (error) {
            std::filesystem::remove(temporary);
            throw std::runtime_error("cannot commit Harness record: " + error.message());
        }
#endif
    }

    std::optional<json> load(const char* collection, const std::string& id) {
        const auto target = path(collection, id);
        std::lock_guard lock(mutex);
        std::ifstream input(target, std::ios::binary);
        if (!input) {
            if (!std::filesystem::exists(target)) return std::nullopt;
            throw std::runtime_error("cannot read Harness record");
        }
        std::ostringstream content;
        content << input.rdbuf();
        return json::parse(content.str());
    }

    std::filesystem::path root;
    std::mutex mutex;
    std::atomic<std::uint64_t> next_temp{1};
};

FileHarnessRecordStore::FileHarnessRecordStore(std::string root_directory)
    : impl_(std::make_unique<Impl>(std::move(root_directory))) {}
FileHarnessRecordStore::~FileHarnessRecordStore() = default;
void FileHarnessRecordStore::save_artifact(const std::string& id, const json& record) {
    impl_->save("artifacts", id, record);
}
std::optional<json> FileHarnessRecordStore::load_artifact(const std::string& id) {
    return impl_->load("artifacts", id);
}
void FileHarnessRecordStore::save_run(const std::string& id, const json& record) {
    impl_->save("runs", id, record);
}
std::optional<json> FileHarnessRecordStore::load_run(const std::string& id) {
    return impl_->load("runs", id);
}

}  // namespace neograph::mcp
