// libcurl HTTP/2 multiplex smoke — 5 parallel POSTs, share connection.
// If multiplexing works, we should see ~max(5 LLM call) wall-clock,
// not 5×LLM. Compare with: curl --parallel 5 vs sequential.

#include <curl/curl.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace {

struct Handle {
    CURL*       easy = nullptr;
    std::string body;
    std::string resp;
    long        status = 0;
    double      total_time = 0;
    curl_slist* hdrs = nullptr;
    int         idx = 0;
};

std::size_t write_cb(char* p, std::size_t s, std::size_t n, std::string* out) {
    out->append(p, s * n);
    return s * n;
}

} // namespace

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) { std::cerr << "OPENAI_API_KEY missing\n"; return 0; }
    std::string auth = std::string("Authorization: Bearer ") + key;

    constexpr int N = 5;
    std::vector<Handle> handles(N);

    CURLM* multi = curl_multi_init();
    // Allow HTTP/2 stream multiplexing (default in 7.62+, explicit anyway).
    curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
    // CURLOPT_PIPEWAIT is per-easy; set on each handle below.

    for (int i = 0; i < N; ++i) {
        auto& h = handles[i];
        h.idx = i;
        h.body = std::string(R"({"model":"gpt-5.4-mini","messages":[{"role":"user","content":"reply word )") +
                 std::to_string(i) + R"("}]})";
        h.easy = curl_easy_init();
        h.hdrs = curl_slist_append(nullptr, "Content-Type: application/json");
        h.hdrs = curl_slist_append(h.hdrs, auth.c_str());

        curl_easy_setopt(h.easy, CURLOPT_URL,
            "https://api.openai.com/v1/chat/completions");
        curl_easy_setopt(h.easy, CURLOPT_HTTPHEADER,    h.hdrs);
        curl_easy_setopt(h.easy, CURLOPT_POST,          1L);
        curl_easy_setopt(h.easy, CURLOPT_POSTFIELDS,    h.body.c_str());
        curl_easy_setopt(h.easy, CURLOPT_POSTFIELDSIZE, h.body.size());
        curl_easy_setopt(h.easy, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(h.easy, CURLOPT_WRITEDATA,     &h.resp);
        curl_easy_setopt(h.easy, CURLOPT_TIMEOUT,       60L);
        curl_easy_setopt(h.easy, CURLOPT_HTTP_VERSION,
            CURL_HTTP_VERSION_2TLS);
        // PIPEWAIT: wait for an existing connection to a host before
        // opening a new one — lets later handles ride the multiplex
        // train of an earlier connection.
        curl_easy_setopt(h.easy, CURLOPT_PIPEWAIT, 1L);
        curl_multi_add_handle(multi, h.easy);
    }

    auto t0 = std::chrono::steady_clock::now();

    int still_running = 0;
    do {
        CURLMcode rc = curl_multi_perform(multi, &still_running);
        if (rc != CURLM_OK) {
            std::cerr << "multi_perform: " << curl_multi_strerror(rc) << "\n";
            break;
        }
        if (still_running) {
            int numfds = 0;
            curl_multi_poll(multi, nullptr, 0, 1000, &numfds);
        }
    } while (still_running > 0);

    auto t1 = std::chrono::steady_clock::now();
    double wall = std::chrono::duration<double>(t1 - t0).count();

    // Drain CURLMSG_DONE messages to capture per-handle status.
    int msgs_left = 0;
    CURLMsg* msg;
    while ((msg = curl_multi_info_read(multi, &msgs_left))) {
        if (msg->msg == CURLMSG_DONE) {
            for (auto& h : handles) {
                if (h.easy == msg->easy_handle) {
                    curl_easy_getinfo(h.easy, CURLINFO_RESPONSE_CODE, &h.status);
                    curl_easy_getinfo(h.easy, CURLINFO_TOTAL_TIME,    &h.total_time);
                    long ver = 0;
                    curl_easy_getinfo(h.easy, CURLINFO_HTTP_VERSION,  &ver);
                    long conn = 0;
                    curl_easy_getinfo(h.easy, CURLINFO_NUM_CONNECTS,  &conn);
                    std::cout << "handle " << h.idx << ": status " << h.status
                              << "  ver=" << ver
                              << "  total=" << h.total_time << "s"
                              << "  new_conn=" << conn
                              << "  resp_bytes=" << h.resp.size() << "\n";
                    break;
                }
            }
        }
    }

    std::cout << "\nwall-clock: " << wall << "s for " << N << " parallel POSTs\n";
    std::cout << "(if multiplexed: should be ~max(per-call latency))\n";

    for (auto& h : handles) {
        curl_multi_remove_handle(multi, h.easy);
        curl_slist_free_all(h.hdrs);
        curl_easy_cleanup(h.easy);
    }
    curl_multi_cleanup(multi);
    curl_global_cleanup();
    return 0;
}
