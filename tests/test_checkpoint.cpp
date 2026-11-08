#include <gtest/gtest.h>
#include <neograph/graph/checkpoint.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <stdexcept>
#include <string>

using namespace neograph::graph;
using json = neograph::json;

class CheckpointTest : public ::testing::Test {
protected:
    InMemoryCheckpointStore store;

    Checkpoint make_cp(const std::string& thread_id, int step,
                       const std::string& node = "test_node") {
        Checkpoint cp;
        cp.id = Checkpoint::generate_id();
        cp.thread_id = thread_id;
        cp.channel_values = json{{"data", step}};
        cp.current_node = node;
        cp.next_nodes = {"__end__"};
        cp.interrupt_phase = CheckpointPhase::Completed;
        cp.step = step;
        cp.timestamp = step * 1000;  // deterministic ordering
        return cp;
    }
};

TEST_F(CheckpointTest, SaveAndLoadLatest) {
    auto cp = make_cp("thread-1", 0);
    store.save(cp);

    auto loaded = store.load_latest("thread-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp.id);
    EXPECT_EQ(loaded->channel_values["data"], 0);
}

TEST_F(CheckpointTest, LoadLatestReturnsNewest) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-1", 1));
    auto cp3 = make_cp("thread-1", 2);
    store.save(cp3);

    auto loaded = store.load_latest("thread-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->id, cp3.id);
    EXPECT_EQ(loaded->step, 2);
}

TEST_F(CheckpointTest, LoadByIdSuccess) {
    auto cp = make_cp("thread-1", 0);
    store.save(cp);

    auto loaded = store.load_by_id(cp.id);
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->thread_id, "thread-1");
}

TEST_F(CheckpointTest, LoadByIdNotFound) {
    auto loaded = store.load_by_id("nonexistent-id");
    EXPECT_FALSE(loaded.has_value());
}

TEST_F(CheckpointTest, LoadLatestNonExistentThread) {
    auto loaded = store.load_latest("nonexistent");
    EXPECT_FALSE(loaded.has_value());
}

TEST_F(CheckpointTest, ListCheckpoints) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-1", 1));
    store.save(make_cp("thread-1", 2));

    auto list = store.list("thread-1");
    EXPECT_EQ(list.size(), 3);
}

TEST_F(CheckpointTest, ListWithLimit) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-1", 1));
    store.save(make_cp("thread-1", 2));

    auto list = store.list("thread-1", 2);
    EXPECT_EQ(list.size(), 2);
}

TEST_F(CheckpointTest, ListIsolatedByThread) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-2", 0));

    auto list1 = store.list("thread-1");
    auto list2 = store.list("thread-2");
    EXPECT_EQ(list1.size(), 1);
    EXPECT_EQ(list2.size(), 1);
}

TEST_F(CheckpointTest, DeleteThread) {
    store.save(make_cp("thread-1", 0));
    store.save(make_cp("thread-1", 1));
    store.save(make_cp("thread-2", 0));

    store.delete_thread("thread-1");

    EXPECT_FALSE(store.load_latest("thread-1").has_value());
    EXPECT_TRUE(store.load_latest("thread-2").has_value());
}

// The latest checkpoint is the one saved last, whatever its step or timestamp,
// and a re-saved ID moves to the newest place, listed once: the contract the
// SQLite and PostgreSQL stores share.
TEST_F(CheckpointTest, LatestIsTheLastSaveAndAResavedIdMovesThere) {
    auto first        = make_cp("t", 4);
    auto second       = make_cp("t", 0);
    second.timestamp  = first.timestamp;
    const auto third  = make_cp("t", 0);
    store.save(first);
    store.save(second);
    ASSERT_TRUE(store.load_latest("t"));
    EXPECT_EQ(store.load_latest("t")->id, second.id);
    store.save(third);

    first.next_nodes = {"edited"};
    store.save(first);
    const auto latest = store.load_latest("t");
    ASSERT_TRUE(latest);
    EXPECT_EQ(latest->id, first.id);
    EXPECT_EQ(latest->next_nodes, std::vector<std::string>{"edited"});
    const auto history = store.list("t");
    ASSERT_EQ(history.size(), 3u);
    EXPECT_EQ(history[0].id, first.id);
    EXPECT_EQ(history[0].next_nodes, std::vector<std::string>{"edited"});
    EXPECT_EQ(history[1].id, third.id);
    EXPECT_EQ(history[2].id, second.id);
    EXPECT_EQ(store.size(), 3u);
}

TEST_F(CheckpointTest, SizeCount) {
    EXPECT_EQ(store.size(), 0);
    store.save(make_cp("t1", 0));
    store.save(make_cp("t2", 0));
    EXPECT_EQ(store.size(), 2);
}

TEST_F(CheckpointTest, GenerateIdUnique) {
    auto id1 = Checkpoint::generate_id();
    auto id2 = Checkpoint::generate_id();
    EXPECT_NE(id1, id2);
    EXPECT_GT(id1.size(), 10);  // UUID-like length
}

// A fresh Checkpoint (either default-constructed or built via make_cp)
// must carry the current schema version, so persistent stores serialize
// the field instead of writing 0 and later mis-classifying it as a
// pre-versioned blob needing migration.
TEST_F(CheckpointTest, FreshCheckpointCarriesCurrentSchemaVersion) {
    Checkpoint fresh;
    EXPECT_EQ(fresh.schema_version, CHECKPOINT_SCHEMA_VERSION);

    auto cp = make_cp("thread-1", 0);
    EXPECT_EQ(cp.schema_version, CHECKPOINT_SCHEMA_VERSION);

    store.save(cp);
    auto loaded = store.load_latest("thread-1");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->schema_version, CHECKPOINT_SCHEMA_VERSION);
}

// Confirms the contract that stores loading legacy blobs (which predate
// the field) can distinguish them by seeing schema_version == 0 and
// take a migration path — the in-memory store round-trips an explicit
// 0 unchanged.
TEST(CheckpointPhaseStrings, AllEnumValuesRoundTrip) {
    // Every enum value must survive to_string → parse_checkpoint_phase
    // so persistent stores can serialize phases as strings and load
    // them back losslessly. Guards against "I added an enum value but
    // forgot one of the two functions" drift.
    for (auto p : {CheckpointPhase::Before, CheckpointPhase::After,
                   CheckpointPhase::Completed, CheckpointPhase::NodeInterrupt,
                   CheckpointPhase::Updated}) {
        EXPECT_EQ(parse_checkpoint_phase(to_string(p)), p)
            << "phase " << to_string(p) << " failed round-trip";
    }
}

TEST(CheckpointPhaseStrings, ParseUnknownThrows) {
    EXPECT_THROW(parse_checkpoint_phase("garbage"), std::invalid_argument);
    EXPECT_THROW(parse_checkpoint_phase(""), std::invalid_argument);
}

TEST_F(CheckpointTest, PreVersionedSentinelRoundTrips) {
    auto cp = make_cp("legacy-thread", 0);
    cp.schema_version = 0;  // simulate a blob deserialized without the field
    store.save(cp);

    auto loaded = store.load_latest("legacy-thread");
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->schema_version, 0)
        << "store must not silently upgrade — migration is the caller's "
           "responsibility so the caller can log / one-shot convert";
}

// The managed-budget journal draws its actor and generation from the SDK's entropy source. A lease carries
// both as 256-bit lower-case hex, and no two leases share one.
namespace {
bool lower_hex_64(const std::string& text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
}  // namespace

TEST_F(CheckpointTest, ManagedBudgetLeaseNoncesAreFresh256BitHex) {
    ManagedBudgetLeaseScope scope;
    scope.owner_scope = "nonce-owner";
    scope.thread_id = "nonce-thread-1";
    scope.graph_identity = "nonce-graph";
    scope.original_ceiling = 100;
    const auto first = store.acquire_managed_budget_lease(scope, "", "");
    scope.thread_id = "nonce-thread-2";
    const auto second = store.acquire_managed_budget_lease(scope, "", "");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_TRUE(lower_hex_64(first->actor_id()));
    EXPECT_TRUE(lower_hex_64(first->bank_generation()));
    EXPECT_NE(first->actor_id(), first->bank_generation());
    EXPECT_NE(first->actor_id(), second->actor_id());
    EXPECT_NE(first->bank_generation(), second->bank_generation());
}

// A single-thread build refuses a synchronous checkpoint store, and a host without a deadline clock origin
// (WebAssembly) refuses a managed budget with a deadline, with these types. A threaded Linux build does neither, so
// the refusals themselves are exercised by wasm/checks.cpp; here the type is what a caller sees.
TEST(SynchronousCheckpointStore, RefusalIsATypedConfigurationFailure) {
    try {
        throw SynchronousCheckpointStoreUnavailable();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("AsyncCheckpointStore"), std::string::npos);
        EXPECT_NE(dynamic_cast<const SynchronousCheckpointStoreUnavailable*>(&error), nullptr);
        EXPECT_NE(dynamic_cast<const ConfigurationFailure*>(&error), nullptr);
    }
}

TEST(ManagedBudgetDeadlineClock, RefusalIsATypedConfigurationFailure) {
    try {
        throw ManagedBudgetDeadlineClockUnavailable();
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("deadline clock"), std::string::npos);
        EXPECT_NE(dynamic_cast<const ManagedBudgetDeadlineClockUnavailable*>(&error), nullptr);
        EXPECT_NE(dynamic_cast<const ConfigurationFailure*>(&error), nullptr)
            << "a deterministic verdict is never retried and never routed to a failure handler";
    }
}

TEST_F(CheckpointTest, T5_EqualFenceSucceedsAndLowerFenceThrows) {
    const std::string owner = "scope-mem-t5";
    const std::string run_id = "run-mem-t5";
    EXPECT_TRUE(store.supports_write_fence());
    EXPECT_EQ(store.load_fence(owner, run_id), 0u);
    EXPECT_EQ(store.raise_fence(owner, run_id, 2), 2u);
    EXPECT_EQ(store.load_fence(owner, run_id), 2u);

    const CheckpointWriteFence f2{owner, run_id, 2};
    const CheckpointWriteFence f1{owner, run_id, 1};

    auto cp = make_cp("thread-t5", 1);
    EXPECT_NO_THROW(store.save(cp, f2));

    PendingWrite pw;
    pw.task_id = "task-t5";
    pw.node_name = "node";
    pw.writes = json::array();
    pw.step = 1;
    EXPECT_NO_THROW(store.put_writes("thread-t5", cp.id, pw, f2));

    auto stale_cp = make_cp("thread-t5", 2);
    try {
        store.save(stale_cp, f1);
        FAIL() << "Expected CheckpointWriteFenceError for wf=1 < sf=2";
    } catch (const CheckpointWriteFenceError& err) {
        EXPECT_EQ(err.refusal().owner_scope, owner);
        EXPECT_EQ(err.refusal().run_id, run_id);
        EXPECT_EQ(err.refusal().writer_fence, 1u);
        EXPECT_EQ(err.refusal().stored_fence, 2u);
    }
    EXPECT_EQ(store.load_latest("thread-t5")->id, cp.id);

    PendingWrite stale_pw = pw;
    stale_pw.task_id = "stale-task";
    EXPECT_THROW(store.put_writes("thread-t5", cp.id, stale_pw, f1), CheckpointWriteFenceError);
    EXPECT_EQ(store.get_writes("thread-t5", cp.id).size(), 1u);

    // wf > sf advances stored_fence to wf; raise_fence with lower min_fence is a no-op max.
    const CheckpointWriteFence f3{owner, run_id, 3};
    auto cp3 = make_cp("thread-t5", 3);
    EXPECT_NO_THROW(store.save(cp3, f3));
    EXPECT_EQ(store.load_fence(owner, run_id), 3u);
    EXPECT_EQ(store.raise_fence(owner, run_id, 2), 3u);
    EXPECT_EQ(store.load_fence(owner, run_id), 3u);

    // clear_writes and delete_thread are fenced for both refusal (wf < sf) and acceptance (wf >= sf).
    EXPECT_THROW(store.clear_writes("thread-t5", cp.id, f2), CheckpointWriteFenceError);
    EXPECT_EQ(store.get_writes("thread-t5", cp.id).size(), 1u);
    EXPECT_NO_THROW(store.clear_writes("thread-t5", cp.id, f3));
    EXPECT_TRUE(store.get_writes("thread-t5", cp.id).empty());

    EXPECT_THROW(store.delete_thread("thread-t5", f2), CheckpointWriteFenceError);
    EXPECT_TRUE(store.load_latest("thread-t5").has_value());
    EXPECT_NO_THROW(store.delete_thread("thread-t5", f3));
    EXPECT_FALSE(store.load_latest("thread-t5").has_value());

    // Managed-budget mutations are fenced for both refusal (wf < sf) and acceptance (wf >= sf).
    ManagedBudgetLeaseScope mb_scope;
    mb_scope.owner_scope = owner;
    mb_scope.thread_id = "mb-thread-t5";
    mb_scope.graph_identity = "mb-graph-t5";
    mb_scope.original_ceiling = 100;

    EXPECT_THROW((void)store.acquire_managed_budget_lease(mb_scope, "", "", f2),
                 CheckpointWriteFenceError);
    auto mb_lease = store.acquire_managed_budget_lease(mb_scope, "", "", f3);
    ASSERT_NE(mb_lease, nullptr);

    const std::string req_digest = "sha256:" + std::string(64, 'a');
    EXPECT_THROW((void)store.begin_managed_budget_effect(mb_lease, "eff-1", 10, req_digest, f2),
                 CheckpointWriteFenceError);
    auto mb_receipt = store.begin_managed_budget_effect(mb_lease, "eff-1", 10, req_digest, f3);
    ASSERT_TRUE(mb_receipt.active());

    sp::Usage usage;
    usage.stage = sp::UsageStage::Final;
    usage.quality = sp::UsageQuality::Consistent;
    usage.input_total = sp::Count{3, sp::Evidence::Derived};
    usage.output_total = sp::Count{2, sp::Evidence::Derived};
    usage.total = sp::Count{5, sp::Evidence::Derived};

    neograph::UsageAccumulator acc;
    ASSERT_TRUE(acc.try_reserve(10, mb_scope.original_ceiling));
    ASSERT_TRUE(acc.remember_provider_effect("eff-1"));
    acc.settle_reservation(10, usage);
    const auto authority = acc.authority_snapshot();

    sp::Completion comp;
    comp.usage = usage;
    auto outcome = std::make_shared<const sp::Outcome>(std::move(comp));

    EXPECT_THROW(store.settle_managed_budget_effect(mb_lease, mb_receipt, outcome, authority, f2),
                 CheckpointWriteFenceError);
    EXPECT_NO_THROW(
        store.settle_managed_budget_effect(mb_lease, mb_receipt, outcome, authority, f3));

    Checkpoint mb_cp = make_cp(mb_scope.thread_id, 1);
    mb_cp.parent_id = "";
    mb_cp.channel_values["provider_managed_budget"] = json{
        {"data", json{
            {"schema", "neograph.graph-managed-bank/v1"},
            {"charged", authority.charged},
            {"reserved", authority.reserved},
            {"provider_effects", json(authority.provider_effects)},
            {"reports", neograph::provider_codec::encode_usage(authority.reports)},
            {"has_report", authority.has_report},
            {"ceiling", mb_scope.original_ceiling},
            {"owner_scope", mb_scope.owner_scope},
            {"thread_id", mb_scope.thread_id},
            {"graph_identity", mb_scope.graph_identity},
        }},
    };

    EXPECT_THROW(store.publish_managed_budget_checkpoint(mb_lease, mb_cp, f2),
                 CheckpointWriteFenceError);
    EXPECT_NO_THROW(store.publish_managed_budget_checkpoint(mb_lease, mb_cp, f3));

    EXPECT_THROW(store.release_managed_budget_lease(mb_lease, f2), CheckpointWriteFenceError);
    EXPECT_NO_THROW(store.release_managed_budget_lease(mb_lease, f3));
}

TEST_F(CheckpointTest, FencedCheckpointStoreRejectsMismatchedExplicitFence) {
    auto inner = std::make_shared<InMemoryCheckpointStore>();
    const CheckpointWriteFence bound{"scope-fenced", "run-fenced", 2};
    FencedCheckpointStore fenced(inner, bound);

    auto cp1 = make_cp("thread-fenced", 1);
    EXPECT_NO_THROW(fenced.save(cp1));
    EXPECT_EQ(inner->load_fence("scope-fenced", "run-fenced"), 2u);

    auto cp2 = make_cp("thread-fenced", 2);
    EXPECT_NO_THROW(fenced.save(cp2, bound));

    const CheckpointWriteFence mismatched_fence{"scope-fenced", "run-fenced", 3};
    EXPECT_THROW(fenced.save(make_cp("thread-fenced", 3), mismatched_fence), std::invalid_argument);

    const CheckpointWriteFence mismatched_run{"scope-fenced", "other-run", 2};
    EXPECT_THROW(fenced.save(make_cp("thread-fenced", 3), mismatched_run), std::invalid_argument);

    const CheckpointWriteFence half_filled{"scope-fenced", "", 2};
    EXPECT_THROW(fenced.save(make_cp("thread-fenced", 3), half_filled), std::invalid_argument);
    EXPECT_THROW((FencedCheckpointStore{inner, half_filled}), std::invalid_argument);
}

TEST_F(CheckpointTest, T3a_WriteGuardHoldsMutexUntilReleasedBeforeRaiseCommits) {
    for (const char* op_kind : {"save", "put_writes", "managed_budget"}) {
        SCOPED_TRACE(op_kind);
        InMemoryCheckpointStore mem_store;
        const std::string owner = "scope-mem-t3a";
        const std::string run_id = "run-mem-t3a";
        const CheckpointWriteFence f1{owner, run_id, 1};

        std::promise<void> at_write_guard;
        std::promise<void> release_promise;
        auto release_write_guard = release_promise.get_future().share();
        std::atomic<bool> guard_active{false};

        mem_store.set_write_guard([&](const CheckpointWriteFence&) {
            if (guard_active.exchange(false)) {
                at_write_guard.set_value();
                release_write_guard.wait();
            }
        });

        ManagedBudgetLeaseScope mb_scope;
        mb_scope.owner_scope = owner;
        mb_scope.thread_id = "t";
        mb_scope.graph_identity = "g";
        mb_scope.original_ceiling = 100;

        guard_active.store(true);
        auto writer_fut = std::async(std::launch::async, [&] {
            if (std::string(op_kind) == "save") {
                mem_store.save(make_cp("t", 1), f1);
            } else if (std::string(op_kind) == "put_writes") {
                PendingWrite pw;
                pw.task_id = "task-1";
                pw.node_name = "n";
                pw.writes = json::array();
                mem_store.put_writes("t", "p0", pw, f1);
            } else {
                (void)mem_store.acquire_managed_budget_lease(mb_scope, "", "", f1);
            }
        });

        at_write_guard.get_future().wait();

        std::promise<void> c2_started;
        auto raiser_fut = std::async(std::launch::async, [&] {
            c2_started.set_value();
            return mem_store.raise_fence(owner, run_id, 2);
        });
        c2_started.get_future().wait();
        EXPECT_EQ(raiser_fut.wait_for(std::chrono::milliseconds(50)),
                  std::future_status::timeout);
        release_promise.set_value();

        EXPECT_NO_THROW(writer_fut.get());
        EXPECT_EQ(raiser_fut.get(), 2u);
        EXPECT_EQ(mem_store.load_fence(owner, run_id), 2u);
        EXPECT_THROW(mem_store.save(make_cp("t", 2), f1), CheckpointWriteFenceError);
    }
}

TEST_F(CheckpointTest, T3b_RaiseGuardBeforeReleaseRefusesConcurrentStaleWriter) {
    for (const char* op_kind : {"save", "put_writes", "managed_budget"}) {
        SCOPED_TRACE(op_kind);
        InMemoryCheckpointStore mem_store;
        const std::string owner = "scope-mem-t3b";
        const std::string run_id = "run-mem-t3b";
        const CheckpointWriteFence f1{owner, run_id, 1};

        ManagedBudgetLeaseScope mb_scope;
        mb_scope.owner_scope = owner;
        mb_scope.thread_id = "t";
        mb_scope.graph_identity = "g";
        mb_scope.original_ceiling = 100;

        std::promise<void> at_raise_guard;
        std::promise<void> release_raise_promise;
        auto release_raise = release_raise_promise.get_future().share();
        std::atomic<bool> raise_guard_fired{false};
        mem_store.set_raise_guard([&](const std::string&, const std::string&, std::uint64_t) {
            if (!raise_guard_fired.exchange(true)) {
                at_raise_guard.set_value();
                release_raise.wait();
            }
        });

        std::promise<void> c2_entering_write;
        std::atomic<bool> before_write_fired{false};
        mem_store.set_before_write_hook([&](const CheckpointWriteFence&) {
            if (!before_write_fired.exchange(true)) {
                c2_entering_write.set_value();
            }
        });

        auto raiser_fut = std::async(std::launch::async, [&] {
            return mem_store.raise_fence(owner, run_id, 2);
        });
        at_raise_guard.get_future().wait();

        auto writer_fut = std::async(std::launch::async, [&] {
            if (std::string(op_kind) == "save") {
                mem_store.save(make_cp("t", 1), f1);
            } else if (std::string(op_kind) == "put_writes") {
                PendingWrite pw;
                pw.task_id = "task-1";
                pw.node_name = "n";
                pw.writes = json::array();
                mem_store.put_writes("t", "p0", pw, f1);
            } else {
                (void)mem_store.acquire_managed_budget_lease(mb_scope, "", "", f1);
            }
        });

        c2_entering_write.get_future().wait();
        EXPECT_EQ(writer_fut.wait_for(std::chrono::milliseconds(50)),
                  std::future_status::timeout);
        release_raise_promise.set_value();

        EXPECT_EQ(raiser_fut.get(), 2u);
        try {
            writer_fut.get();
            FAIL() << "Expected CheckpointWriteFenceError";
        } catch (const CheckpointWriteFenceError& err) {
            EXPECT_EQ(err.refusal().owner_scope, owner);
            EXPECT_EQ(err.refusal().run_id, run_id);
            EXPECT_EQ(err.refusal().writer_fence, 1u);
            EXPECT_EQ(err.refusal().stored_fence, 2u);
        }
        mem_store.set_before_write_hook({});
        mem_store.set_raise_guard({});
        EXPECT_FALSE(mem_store.load_latest("t").has_value());
        EXPECT_TRUE(mem_store.get_writes("t", "p0").empty());
        EXPECT_FALSE(mem_store.requires_managed_budget("t"));
        if (std::string(op_kind) == "managed_budget") {
            auto fresh_lease = mem_store.acquire_managed_budget_lease(
                mb_scope, "", "", CheckpointWriteFence{owner, run_id, 2});
            ASSERT_NE(fresh_lease, nullptr);
            EXPECT_FALSE(fresh_lease->bank_generation().empty());
            EXPECT_EQ(fresh_lease->revision(), 1u);
        }
    }
}

TEST_F(CheckpointTest, SubclassSaveHeldByGateSerializesAgainstRaiseFence) {
    struct GatedInMemorySubclass : InMemoryCheckpointStore {
        using InMemoryCheckpointStore::save;
        std::function<void()> on_save;
        void save(const Checkpoint& cp) override {
            if (on_save) on_save();
            InMemoryCheckpointStore::save(cp);
        }
    };

    GatedInMemorySubclass sub_store;
    const std::string owner = "scope-mem-subclass";
    const std::string run_id = "run-mem-subclass";
    const CheckpointWriteFence f1{owner, run_id, 1};

    std::promise<void> in_subclass_save;
    std::promise<void> release_subclass_save;
    auto release_fut = release_subclass_save.get_future().share();
    std::atomic<bool> gate_armed{true};

    sub_store.on_save = [&] {
        if (gate_armed.exchange(false)) {
            in_subclass_save.set_value();
            release_fut.wait();
        }
    };

    auto save_fut = std::async(std::launch::async, [&] {
        sub_store.save(make_cp("t-sub", 1), f1);
    });
    in_subclass_save.get_future().wait();

    std::promise<void> raise_started;
    auto raise_fut = std::async(std::launch::async, [&] {
        raise_started.set_value();
        return sub_store.raise_fence(owner, run_id, 2);
    });
    raise_started.get_future().wait();
    EXPECT_EQ(raise_fut.wait_for(std::chrono::milliseconds(50)),
              std::future_status::timeout);

    release_subclass_save.set_value();
    EXPECT_NO_THROW(save_fut.get());
    EXPECT_EQ(raise_fut.get(), 2u);
    EXPECT_EQ(sub_store.load_fence(owner, run_id), 2u);
    ASSERT_TRUE(sub_store.load_latest("t-sub").has_value());
    EXPECT_EQ(sub_store.load_latest("t-sub")->step, 1);

    EXPECT_THROW(sub_store.save(make_cp("t-sub", 2), f1), CheckpointWriteFenceError);
    EXPECT_EQ(sub_store.load_latest("t-sub")->step, 1);
}
