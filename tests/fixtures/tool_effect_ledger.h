// Shared fixtures for the durable Tool effect integration tests: an external
// effect ledger that the SQLiteToolEffectBroker never touches, and a raw reader
// for the broker's own receipt rows.
#pragma once

#include <neograph/tool.h>

#include <sqlite3.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <vector>

namespace neograph::test::tool_effects {

inline constexpr const char* kExecutable = "ledger-build:v1";

struct Db {
    sqlite3* raw = nullptr;
    explicit Db(const std::string& path) {
        if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK)
            throw std::runtime_error("cannot open fixture SQLite database");
        sqlite3_busy_timeout(raw, 5000);
    }
    ~Db() { sqlite3_close(raw); }
    Db(const Db&) = delete;
    Db& operator=(const Db&) = delete;
    void exec(const char* sql) const {
        char* error = nullptr;
        if (sqlite3_exec(raw, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            std::string message = error ? error : "unknown";
            sqlite3_free(error);
            throw std::runtime_error("fixture SQL failed: " + message);
        }
    }
};

struct Rendezvous {
    explicit Rendezvous(int parties) : parties(parties) {}
    // Releases every participant once `parties` distinct runs are inside the
    // Tool at the same time. Returns false on timeout (no overlap observed).
    bool arrive(const std::string& run) {
        std::unique_lock lock(mutex);
        inside.insert(run);
        if (static_cast<int>(inside.size()) >= parties) overlapped = true;
        cv.notify_all();
        return cv.wait_for(lock, std::chrono::seconds(20), [&] { return overlapped; });
    }
    const int parties;
    std::mutex mutex;
    std::condition_variable cv;
    std::set<std::string> inside;
    bool overlapped = false;
};

// Holds a physical dispatch after its durable marker has committed, so another
// broker connection can contend for the very same logical slot.
struct DispatchLatch {
    bool hold() {
        std::unique_lock lock(mutex);
        entered = true;
        cv.notify_all();
        return cv.wait_for(lock, std::chrono::seconds(20), [&] { return released; });
    }
    bool wait_until_entered() {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(20), [&] { return entered; });
    }
    void release() {
        std::lock_guard lock(mutex);
        released = true;
        cv.notify_all();
    }
    std::mutex mutex;
    std::condition_variable cv;
    bool entered = false, released = false;
};

// Writes one ledger row per physical execution. Arguments may request
// {"lose": true} (the effect commits, then the response is lost) or
// {"rendezvous": "<run>"} (wait until two runs execute concurrently).
class LedgerTool final : public Tool {
public:
    LedgerTool(std::string ledger_path, std::shared_ptr<Rendezvous> rendezvous,
               std::shared_ptr<DispatchLatch> latch = {}, std::function<void()> after_effect = {})
        : ledger_path_(std::move(ledger_path)), rendezvous_(std::move(rendezvous)),
          latch_(std::move(latch)), after_effect_(std::move(after_effect)) {
        const Db db(ledger_path_);
        db.exec("CREATE TABLE IF NOT EXISTS sdk_effects("
                "id INTEGER PRIMARY KEY AUTOINCREMENT, payload TEXT NOT NULL)");
    }
    ChatTool get_definition() const override {
        return {"ledger", "External ledger write", json{{"type", "object"}}};
    }
    std::string get_name() const override { return "ledger"; }
    std::string execute(const json& arguments) override {
        executions.fetch_add(1);
        if (latch_ && !latch_->hold())
            throw std::runtime_error("physical Tool dispatch was never released");
        if (arguments.contains("rendezvous") && rendezvous_ &&
            !rendezvous_->arrive(arguments.at("rendezvous").get<std::string>()))
            throw std::runtime_error("runs never overlapped inside the Tool");
        sqlite3_int64 id = 0;
        {
            const Db db(ledger_path_);
            sqlite3_stmt* statement = nullptr;
            if (sqlite3_prepare_v2(db.raw, "INSERT INTO sdk_effects(payload) VALUES (?)", -1,
                                   &statement, nullptr) != SQLITE_OK)
                throw std::runtime_error("ledger prepare failed");
            const auto payload = arguments.dump();
            sqlite3_bind_text(statement, 1, payload.c_str(), -1, SQLITE_TRANSIENT);
            const int status = sqlite3_step(statement);
            sqlite3_finalize(statement);
            if (status != SQLITE_DONE) throw std::runtime_error("ledger write failed");
            id = sqlite3_last_insert_rowid(db.raw);
        }
        if (after_effect_) after_effect_();
        if (arguments.value("lose", false))
            throw std::runtime_error("response lost after the ledger commit");
        return json{{"effect_id", id}, {"payload", arguments}}.dump();
    }
    static std::vector<std::string> payloads(const std::string& path) {
        const Db db(path);
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(db.raw, "SELECT payload FROM sdk_effects ORDER BY id", -1,
                               &statement, nullptr) != SQLITE_OK)
            throw std::runtime_error("cannot inspect ledger");
        std::vector<std::string> rows;
        while (sqlite3_step(statement) == SQLITE_ROW)
            rows.emplace_back(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
        sqlite3_finalize(statement);
        return rows;
    }
    static std::size_t count(const std::string& path) { return payloads(path).size(); }

    std::atomic<int> executions{0};

private:
    std::string ledger_path_;
    std::shared_ptr<Rendezvous> rendezvous_;
    std::shared_ptr<DispatchLatch> latch_;
    std::function<void()> after_effect_;
};

struct EffectRow {
    std::string owner, run, thread, task;
    std::int64_t ordinal = 0;
    std::string version, operation, grant, fingerprint, executable;
    std::int64_t first_attempt = 0;
    std::string tool_name, arguments;
    bool has_receipt = false;
    std::string receipt;
    auto slot() const { return std::tie(owner, run, thread, task, ordinal); }
};

inline std::vector<EffectRow> effect_rows(const std::string& path) {
    const Db db(path);
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(
            db.raw,
            "SELECT run, thread, task, ordinal, program_version, operation, grant_id, "
            "binding_fingerprint, executable, first_attempt, tool_name, arguments, receipt, owner "
            "FROM neograph_tool_effects ORDER BY owner, run, thread, task, ordinal",
            -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot inspect effect journal");
    const auto text = [&](int column) {
        const auto* bytes = sqlite3_column_text(statement, column);
        return bytes ? std::string(reinterpret_cast<const char*>(bytes)) : std::string();
    };
    std::vector<EffectRow> rows;
    while (sqlite3_step(statement) == SQLITE_ROW) {
        EffectRow row;
        row.run = text(0);
        row.thread = text(1);
        row.task = text(2);
        row.ordinal = sqlite3_column_int64(statement, 3);
        row.version = text(4);
        row.operation = text(5);
        row.grant = text(6);
        row.fingerprint = text(7);
        row.executable = text(8);
        row.first_attempt = sqlite3_column_int64(statement, 9);
        row.tool_name = text(10);
        row.arguments = text(11);
        row.has_receipt = sqlite3_column_type(statement, 12) != SQLITE_NULL;
        row.receipt = text(12);
        row.owner = text(13);
        rows.push_back(std::move(row));
    }
    sqlite3_finalize(statement);
    return rows;
}

// Faults target exactly the engine-produced stable slot read from the journal.
// No Program test fabricates a dispatch identity or calls the broker directly.
inline void update_effect_field(const std::string& path, const EffectRow& row,
                                const std::string& field, const std::string& value) {
    static const std::set<std::string> allowed{
        "program_version", "operation", "grant_id", "binding_fingerprint",
        "executable", "tool_name", "arguments", "receipt"};
    if (!allowed.contains(field)) throw std::invalid_argument("unsupported journal fault");
    const Db db(path);
    const auto sql = "UPDATE neograph_tool_effects SET " + field +
                     "=? WHERE owner=? AND run=? AND thread=? AND task=? AND ordinal=?";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(db.raw, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK)
        throw std::runtime_error("cannot prepare exact-slot journal fault");
    sqlite3_bind_text(statement, 1, value.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, row.owner.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, row.run.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, row.thread.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, row.task.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 6, row.ordinal);
    const int status = sqlite3_step(statement);
    sqlite3_finalize(statement);
    if (status != SQLITE_DONE || sqlite3_changes(db.raw) != 1)
        throw std::runtime_error("journal fault did not update exactly one original slot");
}


}  // namespace neograph::test::tool_effects
