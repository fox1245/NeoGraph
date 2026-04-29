// 국회의장 (Speaker) — orchestrates the assembly.
//
// 1. Discovers each member via A2A AgentCard.
// 2. Sends the bill text to every member in parallel via A2AClient.
// 3. Parses each member's vote (찬성/반대/기권) from the reply.
// 4. Tallies the result, prints transcript + outcome.
//
// Member URLs come from CLI args:
//   speaker <bill_file> <member_url> [<member_url> ...]

#include <neograph/a2a/client.h>

#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

using namespace neograph;
using namespace neograph::a2a;

namespace {

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("cannot read " + path);
    std::ostringstream ss; ss << f.rdbuf();
    return ss.str();
}

enum class Vote { Yea, Nay, Abstain, Unknown };

const char* vote_label(Vote v) {
    switch (v) {
        case Vote::Yea:     return "찬성";
        case Vote::Nay:     return "반대";
        case Vote::Abstain: return "기권";
        case Vote::Unknown: return "(불명)";
    }
    return "?";
}

/// Best-effort vote extraction. We ask each persona to return their
/// position with a `투표: 찬성/반대/기권` line; this regex lifts that
/// out. If the model rambles instead of complying, we fall back to a
/// keyword search on the whole reply.
Vote parse_vote(const std::string& reply) {
    static const std::regex vote_line(R"(투표\s*[:：]\s*(찬성|반대|기권))");
    std::smatch m;
    if (std::regex_search(reply, m, vote_line)) {
        if (m[1] == "찬성") return Vote::Yea;
        if (m[1] == "반대") return Vote::Nay;
        if (m[1] == "기권") return Vote::Abstain;
    }
    auto pos_yea = reply.find("찬성");
    auto pos_nay = reply.find("반대");
    auto pos_abs = reply.find("기권");
    auto best = std::min({pos_yea, pos_nay, pos_abs});
    if (best == std::string::npos) return Vote::Unknown;
    if (best == pos_yea) return Vote::Yea;
    if (best == pos_nay) return Vote::Nay;
    return Vote::Abstain;
}

struct MemberResult {
    std::string url;
    std::string name;          // from AgentCard
    std::string party;         // from AgentCard description ("AI 국회의원 (X당)")
    std::string reply;
    Vote        vote = Vote::Unknown;
};

std::string extract_party(const std::string& description) {
    static const std::regex r(R"(\(([^)]+)\))");
    std::smatch m;
    if (std::regex_search(description, m, r)) return m[1];
    return "?";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: " << argv[0] << " <bill_file> <member_url> [<member_url> ...]\n";
        return 2;
    }
    std::string bill_text = slurp(argv[1]);
    std::vector<std::string> member_urls;
    for (int i = 2; i < argc; ++i) member_urls.emplace_back(argv[i]);

    std::cout << "\n╔══════════════════════════════════════════════════════╗\n"
              <<   "║          AI 국회 — 본회의 (Plenary Session)          ║\n"
              <<   "╚══════════════════════════════════════════════════════╝\n\n";
    std::cout << "[국회의장] 의안 상정:\n"
              << "----------------------------------------------------------\n"
              << bill_text << "\n"
              << "----------------------------------------------------------\n\n";

    const std::string speaker_question = bill_text +
        "\n\n위 법안에 대해 다음을 답하세요:\n"
        "1) 의견 요지 (3-5문장)\n"
        "2) 마지막 줄에 정확히 다음 형식으로 표시: `투표: 찬성` 또는 `투표: 반대` 또는 `투표: 기권`\n";

    // Round-trip every member in parallel — A2AClient is thread-safe
    // (each call gets its own ephemeral HTTP connection, no shared
    // session state).
    std::vector<std::future<MemberResult>> futs;
    futs.reserve(member_urls.size());
    for (auto& url : member_urls) {
        futs.emplace_back(std::async(std::launch::async, [url, &speaker_question]() {
            MemberResult r;
            r.url = url;
            try {
                A2AClient client(url);
                client.set_timeout(std::chrono::seconds(60));
                auto card = client.fetch_agent_card();
                r.name  = card.name;
                r.party = extract_party(card.description);

                auto task = client.send_message_sync(speaker_question);
                if (!task.history.empty() && !task.history.back().parts.empty()) {
                    for (auto& part : task.history.back().parts) {
                        if (part.kind == "text") r.reply.append(part.text);
                    }
                }
                if (r.reply.empty() && !task.artifacts.empty()) {
                    for (auto& part : task.artifacts.front().parts) {
                        if (part.kind == "text") r.reply.append(part.text);
                    }
                }
                r.vote = parse_vote(r.reply);
            } catch (const std::exception& e) {
                r.reply = std::string("(통신 오류) ") + e.what();
            }
            return r;
        }));
    }

    std::vector<MemberResult> results;
    for (auto& f : futs) results.push_back(f.get());

    // Print transcript.
    std::cout << "[국회의장] 토론 + 투표:\n\n";
    for (auto& r : results) {
        std::cout << "─ " << r.party << " " << r.name
                  << " (" << r.url << ") ─\n"
                  << r.reply << "\n"
                  << "→ 투표 인식: " << vote_label(r.vote) << "\n\n";
    }

    int yea = 0, nay = 0, abs = 0, unk = 0;
    for (auto& r : results) {
        if (r.vote == Vote::Yea) ++yea;
        else if (r.vote == Vote::Nay) ++nay;
        else if (r.vote == Vote::Abstain) ++abs;
        else ++unk;
    }

    std::cout << "[국회의장] 표결 결과:\n"
              << "  찬성: " << yea << "\n"
              << "  반대: " << nay << "\n"
              << "  기권: " << abs << "\n";
    if (unk) std::cout << "  (불명): " << unk << "\n";

    std::cout << "\n[국회의장] ";
    if (yea > nay) std::cout << "본 법안은 가결되었습니다.\n";
    else if (nay > yea) std::cout << "본 법안은 부결되었습니다.\n";
    else std::cout << "찬반 동수입니다 — 본 법안은 부결됩니다 (관례).\n";

    return 0;
}
