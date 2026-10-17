// Pinned identities of representative Program records (#478). A record's id
// is the SHA-256 of its canonical body, so a change in how a body is built
// that changes one byte changes an id here, and stored records would no
// longer validate. The values were produced by the record codecs before
// their bodies moved to the one-document builder.
#include <neograph/program/command_journal.h>
#include <neograph/program/event.h>
#include <neograph/program/graph_migration.h>
#include <neograph/program/invocation.h>
#include <neograph/program/journal.h>
#include <neograph/program/lineage.h>
#include <neograph/program/run_record.h>

#include "../src/program/canonical_json.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using neograph::json;
using namespace neograph::graph;
using namespace neograph::program;

std::string digest(char c) {
    return "sha256:" + std::string(64, c);
}

ProgramEvent event(ProgramEventKind kind, ProgramEventPayload payload) {
    ProgramEvent value;
    value.sequence           = 7;
    value.timestamp_ms       = 1700000000123;
    value.run_id             = "run-golden";
    value.program_version_id = digest('a');
    value.bundle_id          = digest('b');
    value.operation_id       = "root";
    value.core_generation_id = digest('c');
    value.core_run_id        = program_root_core_thread_id(value.run_id, digest('c'));
    value.trace_id           = "trace-golden";
    value.attempt            = 2;
    value.kind               = kind;
    value.payload            = std::move(payload);
    return ProgramEvent::create(std::move(value));
}

ProgramEvent core(TypedGraphEvent payload) {
    return event(ProgramEventKind::Core, std::move(payload));
}

std::vector<std::pair<std::string, ProgramEvent>> events() {
    RunBudget budget{1000, 200, 300, 4, 50, 60, 2, 3, 9};
    RunBudget retries = budget;
    retries.max_node_retries = 5;
    const CoreCheckpointIdentity checkpoint{
        "main", digest('c'), program_root_core_thread_id("run-golden", digest('c')),
        "checkpoint-1", 1};
    return {
        {"started", event(ProgramEventKind::Started, ProgramStartedEvent{budget})},
        {"started_node_retries", event(ProgramEventKind::Started, ProgramStartedEvent{retries})},
        {"operation_started",
         event(ProgramEventKind::OperationStarted, ProgramOperationEvent{"call_core", digest('d')})},
        {"node_start", core(NodeStartEvent{"a", 2, json{{"k", 1}}})},
        {"node_start_bare", core(NodeStartEvent{"b", std::nullopt, json()})},
        {"node_end", core(NodeEndEvent{"a", "b", 3, json{{"x", "y"}}})},
        {"node_end_bare", core(NodeEndEvent{"a", std::nullopt, 0, json()})},
        {"llm_token", core(LlmTokenEvent{"llm", "tok\"en\n\xc3\xa9"})},
        {"channel_write", core(ChannelWriteEvent{"a", "counter", json(42)})},
        {"state_snapshot",
         core(StateSnapshotEvent{json{{"counter", -3}, {"list", json::array({1, 2.5, "s", nullptr})}}})},
        {"routing", core(RoutingEvent{"c", {"x", "y"}, 4, json{{"why", "because"}}})},
        {"routing_bare", core(RoutingEvent{std::nullopt, {}, std::nullopt, json()})},
        {"send_dispatch",
         core(SendDispatchEvent{{Send{"t1", json{{"i", 1}}, "src"}, Send{"t2", json(), ""}}})},
        {"interrupt", core(InterruptEvent{"n", "before", "cp-1", json{{"q", true}}})},
        {"interrupt_bare", core(InterruptEvent{"n", std::nullopt, std::nullopt, json()})},
        {"error", core(ErrorEvent{"n", "boom", json{{"code", 7}}})},
        {"raw", core(RawGraphEvent{GraphEvent{GraphEvent::Type::LLM_TOKEN, "raw-node", json{{"r", 1}}}})},
        {"emit",
         event(ProgramEventKind::Emit,
               ProgramEmitEvent{"emit-op", json{{"value", json::array({"a", 1, nullptr, false})}}})},
        {"checkpoint_published",
         event(ProgramEventKind::CheckpointPublished, ProgramCheckpointEvent{checkpoint})},
        {"terminal",
         event(ProgramEventKind::Terminal, ProgramTerminalEvent{ProgramTerminalStatus::Completed})},
    };
}

const std::map<std::string, std::string> kPinnedEventIds{
    {"started",
     "sha256:78b02c1c54a74476581d1002fde261330e5b6c97abc3b88a1c68a54fcb9acd63"},
    {"started_node_retries",
     "sha256:4deff3795fca0fb75782a4fb272412d6647e5681ade6cc7f19ac82dec9bfe958"},
    {"operation_started",
     "sha256:b16357c6cd9c8bdb19d1914df183cfd21bb6a832a94bb063ce3daf21fc9adf0a"},
    {"node_start",
     "sha256:a71034440d0769ee66f4e0726f605fec21297397c4ebf6754eade435abc98e8b"},
    {"node_start_bare",
     "sha256:2ac3a5a8c1a56973f137ee4b182ccdf556ec4933852c47cfcd7716a95247731b"},
    {"node_end",
     "sha256:0042b5f7e7514b253542008192aa21ee98887ac439c4ee8110ea1865ba992611"},
    {"node_end_bare",
     "sha256:74797307a0a69af2326a3c0c660355c7d316d42c11610524153178869b500b1e"},
    {"llm_token",
     "sha256:9af9017966e39824aa20162705f458cf37447a1479077391504703628239678f"},
    {"channel_write",
     "sha256:b6adf97d4a4ef9bbfcc50655a9d8e2e73df43dd63a93a428596873c541f547c9"},
    {"state_snapshot",
     "sha256:f916970e735717c1bf1f9f5ff4de80c20954030a253f59b205e969fb681f974b"},
    {"routing",
     "sha256:de8d52ef3cdaad06a633f7fea2673708cd5200ba69229bef956dc765c273935b"},
    {"routing_bare",
     "sha256:12225822ed210dba11477c932592e1720ef01291d1f7535e1d8477312a2d6f27"},
    {"send_dispatch",
     "sha256:03850a1296acc92e471f40196f6e06ac015e6d895f0dd7e1ad75849096c44d17"},
    {"interrupt",
     "sha256:e5c4ba286d5bceced034d726467bdfc863b824846516a3cbe68ace7d2f6800c2"},
    {"interrupt_bare",
     "sha256:1242f6d05267ea6fb5b58eaaf2d464db516a7428f091b0169a5e8a483b60961a"},
    {"error",
     "sha256:984c8bc38e3e9cf530b9fb34618916dbcd819a00ce37675ead4b410f7fe38152"},
    {"raw",
     "sha256:5f667316f05d047cc6f9ff187cec179feffbf56849ca343d5b836b9292a2a0e8"},
    {"emit",
     "sha256:a19f059958b5ba32a2bc50e125a5d2167db18984f9648011cfa75eb7a0f36cb6"},
    {"checkpoint_published",
     "sha256:e70513fd5c57ee7a762427fec37c15415bd7ab4fe87f705bf9f9d2971aae6199"},
    {"terminal",
     "sha256:1021c14181146323ae0196e7124152774914b9e224247a11bcf29c854e80fd33"},
};

// Records as the transition store tests build them.
RunBudget run_budget() {
    return {1000, 100, 100, 1, 1, 10, 0, 0, 0};
}

ProgramJournalRecord first_journal() {
    return ProgramJournalRecord::create({{}, "run-golden", digest('a'), digest('b'), 1,
                                         {"root", ContinuationState::Running, 1}, run_budget(),
                                         {}, std::nullopt, 10});
}

ProgramJournalRecord second_journal(const ProgramJournalRecord& first) {
    RunBudget reserved{10, 5, 0, 1, 1, 2, 0, 0, 0};
    reserved.max_node_retries = 3;
    return ProgramJournalRecord::create(
        {first.id, "run-golden", digest('a'), digest('b'), 2,
         {"root", ContinuationState::Running, 1}, run_budget(), reserved,
         CoreCheckpointIdentity{"main", digest('c'),
                                program_root_core_thread_id("run-golden", digest('c')),
                                "checkpoint-2", 1},
         20});
}

RunInvocation invocation() {
    RunInvocation value;
    value.owner_scope        = "owner-golden";
    value.agent_id           = "agent-golden";
    value.program_version_id = digest('a');
    value.run_id             = "run-golden";
    value.budget             = run_budget();
    value.input              = {{"input", 1}, {"text", "h\xc3\xa9llo"}, {"list", json::array({1, -1})}};
    value.message_sequence   = 1;
    value.idempotency_key    = "idem-golden";
    value.correlation_id     = "trace-golden";
    return value;
}

ProgramRunRecord run_record(const ProgramJournalRecord& journal) {
    ProgramRunRecordData data;
    data.owner_scope         = "owner-golden";
    data.run_id              = journal.run_id;
    data.program_version_id  = journal.program_version_id;
    data.bundle_id           = journal.bundle_id;
    data.binding_fingerprint = digest('3');
    data.invocation          = invocation();
    data.continuation        = journal.continuation;
    data.remaining_budget    = journal.remaining_budget;
    data.journal_head        = journal.id;
    data.event_sequence      = 1;
    data.created_at_ms       = 10;
    data.updated_at_ms       = 10;
    return ProgramRunRecord::create(std::move(data));
}

ProgramJavaScriptCommandJournalEntry command_entry(std::uint64_t sequence, bool completed) {
    return ProgramJavaScriptCommandJournalEntry(ProgramJavaScriptCommandJournalEntryData{
        sequence, digest('b'), 1,
        JavaScriptCommand::call_core("journal:golden", "main", json{{"input", 1}}), digest('8'),
        completed ? std::optional<json>{json{{"status", "completed"}, {"value", 2.5}}}
                  : std::nullopt});
}

std::vector<std::pair<std::string, std::string>> record_ids() {
    const auto first   = first_journal();
    const auto second  = second_journal(first);
    const auto run     = run_record(first);
    const auto lineage_id = program_run_lineage_id(run.owner_scope(), run.run_id());
    const auto generation = ProgramRunGeneration::create(ProgramRunGenerationData{
        run.owner_scope(), lineage_id, 1, run.run_id(), run.program_version_id(), run.bundle_id(),
        run.id(), run.journal_head(), std::nullopt, run.created_at_ms()});
    const auto lineage = ProgramRunLineage::create(ProgramRunLineageData{
        run.owner_scope(), lineage_id, run.run_id(), 1, generation.id(), run.id(),
        run.journal_head(), first.remaining_budget, first.inflight_reservation, std::nullopt,
        run.created_at_ms(), run.updated_at_ms()});
    const auto pending   = command_entry(1, false);
    const auto completed = command_entry(2, true);
    return {
        {"journal_first", first.id},
        {"journal_second", second.id},
        {"invocation", invocation().canonical_identity()},
        // Its stored bytes, through a hash so the source stays ASCII.
        {"invocation_bytes", neograph::program::detail::sha256_identity(
                                 "record-identity-golden/v1", invocation().serialize_canonical())},
        {"run_record", run.id()},
        {"generation", generation.id()},
        {"lineage", lineage.id()},
        {"command_pending", pending.id()},
        {"command_pending_coordinate", pending.coordinate_id()},
        {"command_completed", completed.id()},
        {"command_completed_coordinate", completed.coordinate_id()},
    };
}

const std::map<std::string, std::string> kPinnedRecordIds{
    {"journal_first",
     "sha256:6268dc604aef3ad76ef2597085755eb5421b5e266d1cf3fb655e836492a68e20"},
    {"journal_second",
     "sha256:804727df24ffd118ccd9fdee6e72dc29f381fac4b224d32b4ef056298ee62446"},
    {"invocation",
     "sha256:8b67aba6993070581ea757994cd6de95e39dd7ca2c24e18f6ad070c97669d31b"},
    {"invocation_bytes",
     "sha256:088256735fbe073a6acd589f2ba77fab4f7b0da6f7f41ca99bd780a9a17e2fb4"},
    {"run_record",
     "sha256:8ad7bf803d1174d1d704860e8c8706a4d219a551a76ced3d075f3dad5fbe2f9c"},
    {"generation",
     "sha256:b2525d01c7b705ea47e45f46fa727abb5f6788445535b99b3eb8b156ad2672ae"},
    {"lineage",
     "sha256:d8639dc083bcb1af3826446bd779d1b905f9f4b9977a0897e2696568cee57aaa"},
    {"command_pending",
     "sha256:8f858f35db27cd0246b0043c154787d3ae281da4f1cc17adcbd8a15698fb3526"},
    {"command_pending_coordinate",
     "sha256:3a676634282aba4f3bf2513d48d97adb94a895db645e2f5c320b622eb037a075"},
    {"command_completed",
     "sha256:a0bce8122dbaa066af0a677d83eb5f626bcbad6598d19d4393760012601aa100"},
    {"command_completed_coordinate",
     "sha256:3a676634282aba4f3bf2513d48d97adb94a895db645e2f5c320b622eb037a075"},
};

}  // namespace

TEST(ProgramRecordIdentity, EventIdsArePinned) {
    for (const auto& [name, value] : events()) {
        const auto pinned = kPinnedEventIds.find(name);
        if (pinned == kPinnedEventIds.end()) {
            ADD_FAILURE() << "no pinned id for " << name << ": " << value.id;
            continue;
        }
        EXPECT_EQ(value.id, pinned->second) << name;
    }
}

TEST(ProgramRecordIdentity, EventsRoundTripThroughTheirStoredBytes) {
    for (const auto& [name, value] : events()) {
        const auto bytes  = value.serialize_canonical();
        const auto parsed = ProgramEvent::parse(bytes);
        EXPECT_EQ(parsed.id, value.id) << name;
        EXPECT_EQ(parsed.serialize_canonical(), bytes) << name;
    }
}

TEST(ProgramRecordIdentity, RecordIdsArePinned) {
    for (const auto& [name, value] : record_ids()) {
        const auto pinned = kPinnedRecordIds.find(name);
        if (pinned == kPinnedRecordIds.end()) {
            ADD_FAILURE() << "no pinned id for " << name << ": " << value;
            continue;
        }
        EXPECT_EQ(value, pinned->second) << name;
    }
}
