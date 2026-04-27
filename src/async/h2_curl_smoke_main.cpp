// libcurl backend smoke — verify HTTP/2 parity with curl CLI by going
// straight through libcurl_easy. If this works against api.openai.com
// (and our nghttp2 PoC doesn't), the gap is Cloudflare WAF
// fingerprinting; libcurl is the right backend choice.

#include <curl/curl.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

namespace {

std::size_t write_cb(char* ptr, std::size_t size, std::size_t nmemb,
                     std::string* out) {
    out->append(ptr, size * nmemb);
    return size * nmemb;
}

int post(const char* host, const char* path, const std::string& body,
         const char* auth) {
    CURL* curl = curl_easy_init();
    if (!curl) return -1;

    std::string url = std::string("https://") + host + path;
    std::string resp;
    long status = 0;

    struct curl_slist* hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");
    if (auth && *auth) {
        std::string a = std::string("Authorization: Bearer ") + auth;
        hdrs = curl_slist_append(hdrs, a.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL,            url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     hdrs);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,  body.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA,      &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        60L);
    // Force HTTP/2 over TLS (ALPN). Falls back to 1.1 only if ALPN fails.
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION,
                     CURL_HTTP_VERSION_2TLS);

    CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        long http_ver = 0;
        curl_easy_getinfo(curl, CURLINFO_HTTP_VERSION, &http_ver);
        std::cout << host << ": status " << status
                  << "  http_ver=" << http_ver << "\n";
        std::cout << "  body (first 200): " << resp.substr(0, 200) << "\n";
    } else {
        std::cerr << host << ": ERROR " << curl_easy_strerror(rc) << "\n";
        status = -1;
    }

    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);
    return static_cast<int>(status);
}

} // namespace

int main() {
    curl_global_init(CURL_GLOBAL_DEFAULT);

    std::cout << "=== probe 1: api.openai.com ===\n";
    const char* key = std::getenv("OPENAI_API_KEY");
    if (!key || !*key) {
        std::cerr << "OPENAI_API_KEY not set; skipping.\n";
        curl_global_cleanup();
        return 0;
    }
    int rc = post("api.openai.com", "/v1/chat/completions",
        R"({"model":"gpt-5.4-mini","messages":[{"role":"user","content":"reply pong"}]})",
        key);

    curl_global_cleanup();
    return rc == 200 ? 0 : 1;
}
