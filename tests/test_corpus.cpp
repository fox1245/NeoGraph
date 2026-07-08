// Engine side of the engine<->Studio differential corpus (issue #75 M3).
//
// tests/fixtures/topology_corpus/*.json is byte-shared with
// NeoGraph-Studio's tests/corpus/ — the Studio lint (editor/lint.js)
// runs the same fixtures through its JavaScript reimplementation of
// GraphValidator and asserts the same verdicts. A rule change on either
// side that isn't mirrored breaks that side's test, so the two loaders
// cannot silently diverge (differential-testing oracle).
//
// Verdict format: sorted multiset of "CODE:severity" strings.

#include <gtest/gtest.h>
#include <neograph/neograph.h>
#include <neograph/graph/compiler.h>
#include <neograph/graph/validator.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::graph;

namespace {

std::filesystem::path corpus_dir() {
    std::filesystem::path here(__FILE__);
    return here.parent_path() / "fixtures" / "topology_corpus";
}

json load_json(const std::filesystem::path& p) {
    std::ifstream f(p);
    EXPECT_TRUE(f.is_open()) << "cannot open " << p;
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    return json::parse(content);
}

} // namespace

TEST(Corpus, EngineVerdictsMatchManifest) {
    const auto manifest = load_json(corpus_dir() / "manifest.json");
    const auto& expected_map = manifest["expected"];
    size_t checked = 0;

    for (const auto& [fname, expected] : expected_map.items()) {
        const json def = load_json(corpus_dir() / fname);

        CompiledGraph cg;
        ASSERT_NO_THROW(cg = GraphCompiler::compile(def, NodeContext{}))
            << fname << " must parse cleanly (corpus errors are validator-level)";

        auto report = GraphValidator::validate(cg);
        std::vector<std::string> actual;
        for (const auto& d : report.diagnostics) {
            actual.push_back(d.code + ":" + d.severity);
        }
        std::sort(actual.begin(), actual.end());

        std::vector<std::string> want;
        for (const auto& e : expected) want.push_back(e.get<std::string>());
        std::sort(want.begin(), want.end());

        EXPECT_EQ(actual, want) << fname << "\n" << report.summary();

        // Corpus documents are strict; they must also round-trip.
        EXPECT_NO_THROW(GraphCompiler::verify_roundtrip(def, cg)) << fname;
        ++checked;
    }
    EXPECT_GE(checked, 15u);
}
