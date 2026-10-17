// json::parse refuses text that nests arrays and objects deeper than
// json::max_parse_depth. Copying and comparing a value recurse once per level,
// so a value parsed from a few hundred KiB of text could otherwise exhaust
// the stack.
#include <gtest/gtest.h>
#include <neograph/json.h>

#include <cstddef>
#include <functional>
#include <sstream>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#endif

using json = neograph::json;

namespace {

constexpr std::size_t kMax = json::max_parse_depth;

std::string arrays(std::size_t depth) {
    return std::string(depth, '[') + std::string(depth, ']');
}

std::string objects(std::size_t depth) {
    std::string text;
    for (std::size_t level = 0; level < depth; ++level) text += R"({"a":)";
    text += "null";
    return text + std::string(depth, '}');
}

using Work = std::function<void()>;

#if defined(_WIN32)
unsigned __stdcall run_work(void* work) {
    (*static_cast<Work*>(work))();
    return 0;
}
#else
void* run_work(void* work) {
    (*static_cast<Work*>(work))();
    return nullptr;
}
#endif

// Runs work on a new thread with a stack of `bytes`, and waits for it.
void run_on_stack(std::size_t bytes, Work work) {
#if defined(_WIN32)
    const auto thread = reinterpret_cast<HANDLE>(_beginthreadex(
        nullptr, static_cast<unsigned>(bytes), run_work, &work, STACK_SIZE_PARAM_IS_A_RESERVATION,
        nullptr));
    ASSERT_NE(thread, nullptr);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
#else
    pthread_attr_t attributes;
    ASSERT_EQ(pthread_attr_init(&attributes), 0);
    ASSERT_EQ(pthread_attr_setstacksize(&attributes, bytes), 0);
    pthread_t thread;
    ASSERT_EQ(pthread_create(&thread, &attributes, run_work, &work), 0);
    pthread_join(thread, nullptr);
    pthread_attr_destroy(&attributes);
#endif
}

}  // namespace

TEST(JsonParseDepth, AcceptsTheMaximumDepth) {
    const auto value = json::parse(arrays(kMax));
    const json copy  = value;
    EXPECT_EQ(copy, value);
    EXPECT_NO_THROW(json::parse(objects(kMax)));
    EXPECT_NO_THROW(json::parse("[" + objects(kMax - 1) + "]"));
}

TEST(JsonParseDepth, RefusesOneLevelMore) {
    EXPECT_THROW(json::parse(arrays(kMax + 1)), json::parse_error);
    EXPECT_THROW(json::parse(objects(kMax + 1)), json::parse_error);
    EXPECT_THROW(json::parse("[" + objects(kMax) + "]"), json::parse_error);
    std::istringstream stream(arrays(kMax + 1));
    EXPECT_THROW(json::parse(stream), json::parse_error);
}

TEST(JsonParseDepth, RefusesNestingThatWouldExhaustTheStack) {
    // Copying this value overflowed an 8 MiB stack before the limit.
    EXPECT_THROW(json::parse(arrays(300'000)), json::parse_error);
}

TEST(JsonParseDepth, ValuesAtTheLimitFitASmallThreadStack) {
    // 512 KiB is what a macOS secondary thread gets, the smallest default
    // stack of a platform NeoGraph ships for.
    for (const auto& text : {arrays(kMax), objects(kMax)}) {
        const auto value = json::parse(text);
        bool       same  = false;
        run_on_stack(512 * 1024, [&] {
            const json copy   = value;
            const json parsed = json::parse(text);
            same = copy == value && parsed == value && parsed.dump() == value.dump();
        });
        EXPECT_TRUE(same);
    }
}

TEST(JsonParseDepth, TakesALimitFromTheCaller) {
    // A smaller limit, for a thread with a small stack.
    EXPECT_NO_THROW(json::parse(arrays(16), 16));
    EXPECT_THROW(json::parse(arrays(17), 16), json::parse_error);
    EXPECT_THROW(json::parse("[" + objects(16) + "]", 16), json::parse_error);
    // A larger one, for text the caller trusts.
    EXPECT_TRUE(json::parse(arrays(kMax + 1), kMax + 1).is_array());
    EXPECT_THROW(json::parse(arrays(kMax + 2), kMax + 1), json::parse_error);
    std::istringstream deeper(arrays(kMax + 1));
    EXPECT_TRUE(json::parse(deeper, kMax + 1).is_array());
    std::istringstream shallower(arrays(17));
    EXPECT_THROW(json::parse(shallower, 16), json::parse_error);
}

TEST(JsonParseDepth, ALimitOfZeroAdmitsOnlyScalars) {
    EXPECT_EQ(json::parse(R"("text")", 0).get<std::string>(), "text");
    EXPECT_THROW(json::parse("[]", 0), json::parse_error);
    EXPECT_THROW(json::parse("{}", 0), json::parse_error);
}

TEST(JsonParseDepth, TheErrorNamesTheLimit) {
    try {
        json::parse(arrays(9), 8);
        FAIL() << "nine levels passed a limit of eight";
    } catch (const json::parse_error& error) {
        EXPECT_NE(std::string(error.what()).find("deeper than 8 levels"), std::string::npos)
            << error.what();
    }
}

TEST(JsonParseDepth, CountsOnlyTheEnclosingContainers) {
    // Many siblings, scalars and keys add no depth.
    std::string wide = "[";
    for (int item = 0; item < 100'000; ++item) wide += item ? ",[1,{\"k\":2}]" : "[1,{\"k\":2}]";
    EXPECT_EQ(json::parse(wide + "]").size(), 100'000u);
    // A branch at the limit, then siblings, then a second branch at the limit.
    EXPECT_NO_THROW(json::parse("[" + arrays(kMax - 1) + ",1,{\"a\":[]}," + arrays(kMax - 1) + "]"));
    // A shallow branch, then one that goes a level too deep.
    EXPECT_THROW(json::parse("[" + arrays(10) + "," + arrays(kMax) + "]"), json::parse_error);
    // A scalar document has no containers at all.
    EXPECT_EQ(json::parse("7").get<int>(), 7);
}
