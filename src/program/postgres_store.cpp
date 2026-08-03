#include <neograph/program/postgres_store.h>

#include <libpq-fe.h>

#include <algorithm>
#include <limits>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace neograph::program {
namespace {

class PgResult final {
public:
    explicit PgResult(PGresult* result) : result_(result) {}
    PgResult(const PgResult&) = delete;
    PgResult& operator=(const PgResult&) = delete;
    PgResult(PgResult&& other) noexcept : result_(std::exchange(other.result_, nullptr)) {}
    PgResult& operator=(PgResult&& other) noexcept {
        if (this != &other) {
            clear();
            result_ = std::exchange(other.result_, nullptr);
        }
        return *this;
    }
    ~PgResult() { clear(); }

    PGresult* get() const noexcept { return result_; }

private:
    void clear() noexcept {
        if (result_) PQclear(result_);
        result_ = nullptr;
    }

    PGresult* result_ = nullptr;
};

[[noreturn]] void throw_pg(PGconn* connection, std::string_view operation) {
    std::string message(operation);
    message += ": ";
    if (connection) {
        const auto* detail = PQerrorMessage(connection);
        if (detail && *detail) message += detail;
    }
    throw std::runtime_error(std::move(message));
}

void check_result(PGconn* connection, const PgResult& result, std::string_view operation) {
    if (!result.get()) throw_pg(connection, operation);
    const auto status = PQresultStatus(result.get());
    if (status == PGRES_COMMAND_OK || status == PGRES_TUPLES_OK) return;

    std::string message(operation);
    message += ": ";
    const auto* detail = PQresultErrorMessage(result.get());
    if (detail && *detail) {
        message += detail;
    } else if (connection) {
        const auto* connection_detail = PQerrorMessage(connection);
        if (connection_detail) message += connection_detail;
    }
    throw std::runtime_error(std::move(message));
}

PgResult exec_sql(PGconn* connection, std::string_view sql) {
    PgResult result(PQexec(connection, std::string(sql).c_str()));
    check_result(connection, result, "PostgreSQL command");
    return result;
}

PgResult exec_params(PGconn* connection, std::string_view sql,
                    const std::vector<std::string>& params) {
    std::vector<const char*> values;
    values.reserve(params.size());
    for (const auto& param : params) values.push_back(param.c_str());
    PgResult result(PQexecParams(connection, std::string(sql).c_str(),
                                 static_cast<int>(values.size()), nullptr,
                                 values.empty() ? nullptr : values.data(), nullptr, nullptr, 0));
    check_result(connection, result, "PostgreSQL parameterized command");
    return result;
}

std::optional<std::string> select_bytes(PGconn* connection, std::string_view sql,
                                        const std::vector<std::string>& params) {
    auto result = exec_params(connection, sql, params);
    if (PQntuples(result.get()) == 0) return std::nullopt;
    if (PQntuples(result.get()) != 1 || PQnfields(result.get()) != 1)
        throw std::runtime_error("PostgreSQL ProgramStore returned an invalid row shape");
    if (PQgetisnull(result.get(), 0, 0))
        throw std::runtime_error("PostgreSQL ProgramStore returned a null canonical value");
    return std::string(PQgetvalue(result.get(), 0, 0),
                       static_cast<std::size_t>(PQgetlength(result.get(), 0, 0)));
}

std::uint64_t result_count(const PgResult& result) {
    const auto* value = PQcmdTuples(result.get());
    if (!value || !*value) return 0;
    return std::stoull(value);
}

class Transaction final {
public:
    explicit Transaction(PGconn* connection) : connection_(connection) {
        exec_sql(connection_, "BEGIN");
    }
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    ~Transaction() {
        if (!committed_) {
            try {
                exec_sql(connection_, "ROLLBACK");
            } catch (...) {
            }
        }
    }
    void commit() {
        exec_sql(connection_, "COMMIT");
        committed_ = true;
    }

private:
    PGconn* connection_ = nullptr;
    bool    committed_  = false;
};

std::optional<ProgramActivation> load_activation(PGconn* connection,
                                                  std::string_view owner_scope) {
    const auto bytes = select_bytes(
        connection,
        "SELECT canonical_bytes FROM neograph_program_activations WHERE owner_scope = $1",
        {std::string(owner_scope)});
    if (!bytes) return std::nullopt;
    return ProgramActivation::parse(*bytes);
}

void verify_existing(const std::optional<std::string>& existing,
                     std::string_view                    expected,
                     std::string_view                    kind) {
    if (existing && *existing != expected)
        throw std::invalid_argument("PostgreSQL ProgramStore " + std::string(kind) +
                                    " content identity collision");
}

}  // namespace

struct PostgreSQLProgramStore::Impl {
    explicit Impl(std::string connection_string) : connection_string(std::move(connection_string)) {}
    ~Impl() {
        if (connection) PQfinish(connection);
    }

    std::string       connection_string;
    PGconn*           connection = nullptr;
    mutable std::mutex mutex;
};

PostgreSQLProgramStore::PostgreSQLProgramStore(std::string connection_string)
    : impl_(std::make_unique<Impl>(std::move(connection_string))) {
    if (impl_->connection_string.empty())
        throw std::invalid_argument("PostgreSQL ProgramStore connection string must not be empty");
    impl_->connection = PQconnectdb(impl_->connection_string.c_str());
    if (!impl_->connection || PQstatus(impl_->connection) != CONNECTION_OK) {
        const auto* detail = impl_->connection ? PQerrorMessage(impl_->connection) : nullptr;
        std::string message = "PostgreSQL ProgramStore connection failed";
        if (detail && *detail) {
            message += ": ";
            message += detail;
        }
        if (impl_->connection) {
            PQfinish(impl_->connection);
            impl_->connection = nullptr;
        }
        throw std::runtime_error(std::move(message));
    }

    try {
        exec_sql(impl_->connection,
                 "CREATE TABLE IF NOT EXISTS neograph_program_bundles ("
                 "id TEXT PRIMARY KEY NOT NULL, canonical_bytes TEXT NOT NULL);"
                 "CREATE TABLE IF NOT EXISTS neograph_program_versions ("
                 "id TEXT PRIMARY KEY NOT NULL, bundle_id TEXT NOT NULL, owner_scope TEXT NOT NULL, "
                 "canonical_bytes TEXT NOT NULL);"
                 "CREATE TABLE IF NOT EXISTS neograph_program_activations ("
                 "owner_scope TEXT PRIMARY KEY NOT NULL, generation BIGINT NOT NULL, "
                 "active_version_id TEXT NOT NULL, policy_snapshot_hash TEXT NOT NULL, "
                 "canonical_bytes TEXT NOT NULL);"
                 "CREATE INDEX IF NOT EXISTS neograph_program_versions_owner_idx "
                 "ON neograph_program_versions(owner_scope);");
    } catch (...) {
        PQfinish(impl_->connection);
        impl_->connection = nullptr;
        throw;
    }
}

PostgreSQLProgramStore::PostgreSQLProgramStore(PostgreSQLProgramStore&&) noexcept = default;
PostgreSQLProgramStore& PostgreSQLProgramStore::operator=(PostgreSQLProgramStore&&) noexcept = default;
PostgreSQLProgramStore::~PostgreSQLProgramStore() = default;

void PostgreSQLProgramStore::publish_admitted(const ProgramBundle& bundle,
                                              const ProgramVersion& version) {
    if (version.bundle_id() != bundle.id())
        throw std::invalid_argument("Program version does not bind the published bundle");
    const auto bundle_bytes  = bundle.serialize_canonical();
    const auto version_bytes = version.serialize_canonical();

    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    verify_existing(select_bytes(impl_->connection,
                                 "SELECT canonical_bytes FROM neograph_program_bundles "
                                 "WHERE id = $1",
                                 {bundle.id()}),
                    bundle_bytes, "bundle");
    verify_existing(select_bytes(impl_->connection,
                                 "SELECT canonical_bytes FROM neograph_program_versions "
                                 "WHERE id = $1",
                                 {version.id()}),
                    version_bytes, "version");
    exec_params(impl_->connection,
                "INSERT INTO neograph_program_bundles(id, canonical_bytes) VALUES($1, $2) "
                "ON CONFLICT(id) DO NOTHING",
                {bundle.id(), bundle_bytes});
    exec_params(impl_->connection,
                "INSERT INTO neograph_program_versions(id, bundle_id, owner_scope, canonical_bytes) "
                "VALUES($1, $2, $3, $4) ON CONFLICT(id) DO NOTHING",
                {version.id(), version.bundle_id(), version.ownership_scope(), version_bytes});
    transaction.commit();
}

std::optional<ProgramBundle> PostgreSQLProgramStore::get_bundle(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto bytes = select_bytes(impl_->connection,
                                    "SELECT canonical_bytes FROM neograph_program_bundles "
                                    "WHERE id = $1",
                                    {std::string(id)});
    if (!bytes) return std::nullopt;
    return ProgramBundle::parse(*bytes);
}

std::optional<ProgramVersion> PostgreSQLProgramStore::get_version(std::string_view id) const {
    std::lock_guard lock(impl_->mutex);
    const auto bytes = select_bytes(impl_->connection,
                                    "SELECT canonical_bytes FROM neograph_program_versions "
                                    "WHERE id = $1",
                                    {std::string(id)});
    if (!bytes) return std::nullopt;
    return ProgramVersion::parse(*bytes);
}

std::optional<ProgramActivation>
PostgreSQLProgramStore::get_activation(std::string_view owner_scope) const {
    std::lock_guard lock(impl_->mutex);
    return load_activation(impl_->connection, owner_scope);
}

ProgramActivationResult PostgreSQLProgramStore::compare_activate(
    std::string_view owner_scope,
    std::uint64_t    expected_generation,
    std::string_view version_id,
    std::string_view policy_snapshot_hash) {
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    const auto version_bytes = select_bytes(
        impl_->connection,
        "SELECT canonical_bytes FROM neograph_program_versions WHERE id = $1",
        {std::string(version_id)});
    if (!version_bytes)
        throw std::invalid_argument("Cannot activate an unpublished Program version");
    const auto version = ProgramVersion::parse(*version_bytes);
    if (version.ownership_scope() != owner_scope)
        throw std::invalid_argument("Program activation owner scope does not match the version");

    const auto current = load_activation(impl_->connection, owner_scope);
    const auto generation = current ? current->generation() : std::uint64_t{0};
    if (generation != expected_generation) {
        transaction.commit();
        return ProgramActivationResult::Conflict;
    }
    if (current && current->active_version_id() == version_id &&
        current->policy_snapshot_hash() == policy_snapshot_hash) {
        transaction.commit();
        return ProgramActivationResult::AlreadyPresent;
    }
    if (expected_generation == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Program activation generation exhausted");

    auto activation = ProgramActivation::create(
        ProgramActivationData{std::string(owner_scope), std::string(version_id),
                              expected_generation + 1, std::string(policy_snapshot_hash)});
    const auto activation_bytes = activation.serialize_canonical();
    exec_params(
        impl_->connection,
        "INSERT INTO neograph_program_activations(owner_scope, generation, active_version_id, "
        "policy_snapshot_hash, canonical_bytes) VALUES($1, $2, $3, $4, $5) "
        "ON CONFLICT(owner_scope) DO UPDATE SET generation = EXCLUDED.generation, "
        "active_version_id = EXCLUDED.active_version_id, "
        "policy_snapshot_hash = EXCLUDED.policy_snapshot_hash, "
        "canonical_bytes = EXCLUDED.canonical_bytes",
        {std::string(owner_scope), std::to_string(activation.generation()),
         activation.active_version_id(), activation.policy_snapshot_hash(), activation_bytes});
    transaction.commit();
    return ProgramActivationResult::Activated;
}

std::vector<ProgramVersion>
PostgreSQLProgramStore::list_versions(std::string_view owner_scope) const {
    std::lock_guard lock(impl_->mutex);
    auto result = exec_params(
        impl_->connection,
        "SELECT canonical_bytes FROM neograph_program_versions WHERE owner_scope = $1 ORDER BY id",
        {std::string(owner_scope)});
    std::vector<ProgramVersion> versions;
    versions.reserve(static_cast<std::size_t>(PQntuples(result.get())));
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        if (PQgetisnull(result.get(), row, 0))
            throw std::runtime_error("PostgreSQL ProgramStore returned a null version");
        versions.push_back(ProgramVersion::parse(
            std::string(PQgetvalue(result.get(), row, 0),
                        static_cast<std::size_t>(PQgetlength(result.get(), row, 0)))));
    }
    return versions;
}

ProgramRetentionReport PostgreSQLProgramStore::collect_garbage(
    std::string_view owner_scope,
    const std::vector<std::string>& pinned_version_ids) {
    std::lock_guard lock(impl_->mutex);
    Transaction transaction(impl_->connection);
    std::set<std::string, std::less<>> keep(pinned_version_ids.begin(), pinned_version_ids.end());
    if (const auto active = load_activation(impl_->connection, owner_scope))
        keep.insert(active->active_version_id());

    auto result = exec_params(
        impl_->connection,
        "SELECT id FROM neograph_program_versions WHERE owner_scope = $1 ORDER BY id",
        {std::string(owner_scope)});
    std::vector<std::string> remove;
    for (int row = 0; row < PQntuples(result.get()); ++row) {
        if (PQgetisnull(result.get(), row, 0)) continue;
        const std::string id(PQgetvalue(result.get(), row, 0),
                             static_cast<std::size_t>(PQgetlength(result.get(), row, 0)));
        if (!keep.contains(id)) remove.push_back(id);
    }

    ProgramRetentionReport report;
    for (const auto& id : remove) {
        const auto deleted = exec_params(
            impl_->connection, "DELETE FROM neograph_program_versions WHERE id = $1", {id});
        report.versions_removed += result_count(deleted);
    }
    const auto bundles = exec_sql(
        impl_->connection,
        "DELETE FROM neograph_program_bundles b WHERE NOT EXISTS ("
        "SELECT 1 FROM neograph_program_versions v WHERE v.bundle_id = b.id)");
    report.bundles_removed = result_count(bundles);
    transaction.commit();
    return report;
}

}  // namespace neograph::program
