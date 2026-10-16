// json documents come from a per-thread block cache (#471). A value may be
// made on one thread and read and dropped on another, and a thread may end
// with blocks still cached.
#include <gtest/gtest.h>
#include <neograph/json.h>

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using json = neograph::json;

namespace {

// Small and large members, so the value spans every cached block size and
// some larger blocks the cache passes straight to malloc.
json sample(std::size_t index) {
    json value = {{"index", static_cast<unsigned long long>(index)},
                  {"name", "value-" + std::to_string(index)},
                  {"ratio", static_cast<double>(index) / 7.0},
                  {"flags", json::array({true, false, nullptr})}};
    auto items = json::array();
    for (std::size_t item = 0; item < index % 97; ++item)
        items.push_back(json{{"item", static_cast<long long>(item) - 40}});
    value["items"] = std::move(items);
    value["text"]  = std::string(index % 5 == 0 ? 6000 : index % 300, 'x');
    return value;
}

}  // namespace

TEST(JsonDocumentMemory, ValuesMadeOnOneThreadAreReadAndDroppedOnOthers) {
    constexpr std::size_t count = 2000;
    std::vector<json>        values;
    std::vector<std::string> expected;
    std::thread              maker([&] {
        values.reserve(count);
        expected.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            values.push_back(sample(index));
            expected.push_back(values.back().dump());
        }
    });
    maker.join();

    std::atomic<std::size_t> mismatches{0};
    std::vector<std::thread> droppers;
    for (std::size_t worker = 0; worker < 4; ++worker) {
        droppers.emplace_back([&, worker] {
            for (std::size_t index = worker; index < count; index += 4) {
                const auto parsed = json::parse(values[index].dump());
                if (values[index].dump() != expected[index] || parsed.dump() != expected[index])
                    mismatches.fetch_add(1);
                values[index] = json();
            }
        });
    }
    for (auto& dropper : droppers) dropper.join();
    EXPECT_EQ(mismatches.load(), 0u);
}

TEST(JsonDocumentMemory, ThreadsMayEndWithCachedBlocks) {
    std::vector<std::thread> threads;
    for (std::size_t worker = 0; worker < 16; ++worker) {
        threads.emplace_back([worker] {
            for (std::size_t index = 0; index < 500; ++index) {
                auto value = sample(worker * 500 + index);
                ASSERT_EQ(json::parse(value.dump()).dump(), value.dump());
            }
        });
    }
    for (auto& thread : threads) thread.join();
    // The ending threads released their caches; this one still allocates.
    EXPECT_EQ(sample(42).dump(), sample(42).dump());
}
