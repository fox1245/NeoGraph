// M4 schema-evolution gate (issue #75): backward-compatibility check of
// export_schema() against the checked-in snapshot
// (tests/fixtures/schema_snapshot.json).
//
// The rule set is the decidable "add-only" subset of subschema checking
// (JSON Subschema, arXiv:1911.12651; StableHLO/VHLO practice):
//   - a node type / reducer / condition present in the snapshot must
//     still exist;
//   - a declared config property must not disappear, and the
//     "required" list must not grow (old documents would be rejected);
//   - a *closed* condition's label set must not change at all (E10
//     route-completeness of existing documents depends on it);
//   - declared node effects must not change for snapshot types
//     (changed effects re-judge existing graphs).
//
// If this test fails, the change is version-visible: bump
// kSupportedSchemaVersion, provide upgrade_vN_to_vN+1(), and
// regenerate the snapshot (./example_export_schema >
// tests/fixtures/schema_snapshot.json) in the SAME reviewed commit.
// CI runs this test, so an incompatible schema change cannot merge
// silently.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/loader.h>

#include <filesystem>
#include <fstream>
#include <set>
#include <string>

using namespace neograph;
using namespace neograph::graph;

namespace {

json load_snapshot() {
    std::filesystem::path here(__FILE__);
    std::ifstream f(here.parent_path() / "fixtures" / "schema_snapshot.json");
    EXPECT_TRUE(f.is_open());
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    return json::parse(content);
}

std::set<std::string> string_set(const json& arr) {
    std::set<std::string> s;
    for (const auto& v : arr) s.insert(v.get<std::string>());
    return s;
}

} // namespace

TEST(SchemaEvolution, CurrentSchemaIsBackwardCompatibleWithSnapshot) {
    const json snap = load_snapshot();
    const json cur  = NodeFactory::instance().export_schema();

    // Registries are process-global and other tests register extra
    // types — the check is containment (snapshot ⊆ current), so test
    // ordering cannot break it.

    // Reducers / conditions: removal is breaking.
    for (const auto& r : string_set(snap["reducers"])) {
        EXPECT_TRUE(string_set(cur["reducers"]).count(r))
            << "reducer '" << r << "' was removed — breaking change";
    }
    for (const auto& c : string_set(snap["conditions"])) {
        EXPECT_TRUE(string_set(cur["conditions"]).count(c))
            << "condition '" << c << "' was removed — breaking change";
    }

    // Node types: removal breaking; required must not grow; declared
    // properties must not disappear.
    for (const auto& [type, snap_schema] : snap["node_types"].items()) {
        ASSERT_TRUE(cur["node_types"].contains(type))
            << "node type '" << type << "' was removed — breaking change";
        const json cur_schema = cur["node_types"][type];

        std::set<std::string> snap_req, cur_req;
        if (snap_schema.contains("required")) snap_req = string_set(snap_schema["required"]);
        if (cur_schema.contains("required"))  cur_req  = string_set(cur_schema["required"]);
        for (const auto& r : cur_req) {
            EXPECT_TRUE(snap_req.count(r))
                << "node type '" << type << "': new required field '" << r
                << "' — old documents would be rejected (breaking)";
        }
        if (snap_schema.contains("properties")) {
            for (const auto& [pk, pv] : snap_schema["properties"].items()) {
                (void)pv;
                EXPECT_TRUE(cur_schema.contains("properties")
                            && cur_schema["properties"].contains(pk))
                    << "node type '" << type << "': property '" << pk
                    << "' was removed — strict mode would now refuse old documents";
            }
        }
    }

    // Closed conditions: label set is a contract E10 enforces on user
    // routes — it must not change without a version bump.
    for (const auto& [cname, snap_spec] : snap["condition_specs"].items()) {
        ASSERT_TRUE(cur["condition_specs"].contains(cname))
            << "condition spec '" << cname << "' was removed";
        const json cur_spec = cur["condition_specs"][cname];
        EXPECT_EQ(snap_spec["open"].get<bool>(), cur_spec["open"].get<bool>())
            << "condition '" << cname << "': open/closed flipped";
        if (!snap_spec["open"].get<bool>()) {
            EXPECT_EQ(string_set(snap_spec["labels"]), string_set(cur_spec["labels"]))
                << "closed condition '" << cname
                << "': label set changed — existing routes re-judged (breaking)";
        }
    }

    // Effects: a snapshot type's declared effects must be stable.
    for (const auto& [type, snap_eff] : snap["node_effects"].items()) {
        ASSERT_TRUE(cur["node_effects"].contains(type))
            << "node type '" << type << "' lost its effect contract";
        EXPECT_EQ(string_set(snap_eff["reads"]),
                  string_set(cur["node_effects"][type]["reads"])) << type;
        EXPECT_EQ(string_set(snap_eff["writes"]),
                  string_set(cur["node_effects"][type]["writes"])) << type;
    }

    // Topology envelope: properties must not disappear.
    for (const auto& [pk, pv] : snap["topology"]["properties"].items()) {
        (void)pv;
        EXPECT_TRUE(cur["topology"]["properties"].contains(pk))
            << "topology property '" << pk << "' was removed — breaking change";
    }
}
