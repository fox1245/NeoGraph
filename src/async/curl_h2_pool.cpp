// CurlH2Pool — see header for design. Implementation notes:
//
//   * One worker thread per pool instance. Worker runs `curl_multi`
//     in a poll loop and serves the submission queue from the same
//     thread (no cross-thread access to the multi handle, so we can
//     skip the curl_multi mutex documentation hand-wringing).
//
//   * Submission flow:
//       caller    : queues request → cv.notify → suspends on asio handler
//       worker    : pops request → builds easy handle → multi_add_handle
//       worker    : multi_perform / multi_poll until CURLMSG_DONE
//       worker    : asio::post(caller_ex, [handler, response] { ... })
//
//   * Cleanup: dtor signals stop, joins worker, drains pending
//     handlers with a synthetic timeout-style HttpResponse so caller
//     coroutines unblock.

#include <neograph/async/curl_h2_pool.h>

#include <stdexcept>

#ifndef NEOGRAPH_HAVE_LIBCURL
// Stub implementation when the libcurl backend is disabled at build
// time. Keeps the destructor symbol available for SchemaProvider's
// unique_ptr<CurlH2Pool> member; constructor + async_post throw the
// same way the runtime would on a misconfigured prefer_libcurl=true.
namespace neograph::async {
struct CurlH2Pool::Impl {};
CurlH2Pool::CurlH2Pool() : impl_(nullptr) {
    throw std::runtime_error(
        "CurlH2Pool: libcurl backend not compiled "
        "(rebuild with -DNEOGRAPH_USE_LIBCURL=ON)");
}
CurlH2Pool::~CurlH2Pool() = default;
asio::awaitable<HttpResponse> CurlH2Pool::async_post(
    std::string, std::string,
    std::vector<std::pair<std::string, std::string>>,
    RequestOptions) {
    throw std::runtime_error("CurlH2Pool: libcurl backend not compiled");
    co_return HttpResponse{};
}
} // namespace neograph::async
#else  // NEOGRAPH_HAVE_LIBCURL

#include <curl/curl.h>

#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/async_result.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace neograph::async {

namespace {

// Per-request state owned by the worker until completion. The handler
// is a type-erased `void(HttpResponse, std::exception_ptr)` callable;
// we store it as a std::function so async_initiate can capture our
// asio handler with whatever wrappers it brings.
struct Pending {
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    long timeout_seconds = 0;

    asio::any_io_executor caller_ex;
    std::function<void(HttpResponse, std::exception_ptr)> on_done;

    // Filled by the worker as the request runs.
    std::string resp_body;
    std::vector<std::pair<std::string, std::string>> resp_headers;
    long status = 0;
    std::string error;

    CURL*              easy        = nullptr;
    struct curl_slist* curl_hdrs   = nullptr;
};

std::size_t write_body_cb(char* p, std::size_t s, std::size_t n, void* up) {
    auto* st = static_cast<Pending*>(up);
    st->resp_body.append(p, s * n);
    return s * n;
}

// libcurl emits each HTTP header as one line including CRLF. Skip
// the status line ("HTTP/...") and any trailing blank line.
std::size_t header_line_cb(char* p, std::size_t s, std::size_t n, void* up) {
    auto* st = static_cast<Pending*>(up);
    std::string_view line(p, s * n);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.empty() || line.substr(0, 5) == "HTTP/") return s * n;
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return s * n;
    std::string name(line.substr(0, colon));
    std::string value(line.substr(colon + 1));
    auto first = value.find_first_not_of(" \t");
    if (first != std::string::npos) value = value.substr(first);
    else value.clear();
    st->resp_headers.emplace_back(std::move(name), std::move(value));
    return s * n;
}

} // namespace

struct CurlH2Pool::Impl {
    CURLM*      multi   = nullptr;
    std::thread worker;

    std::mutex                            mu;
    std::condition_variable               cv;
    std::deque<std::unique_ptr<Pending>>  queue;          // submitted, not yet on multi
    std::vector<std::unique_ptr<Pending>> active;         // owns while in-flight on multi
    std::atomic<bool>                     stop{false};

    void worker_loop() {
        // 1.62+ default; explicit so older runtimes also multiplex.
        curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
        // DIAG: allow multiple H/2 connections per host (default 0 = 1).
        // Lets libcurl spread N parallel streams across M conns instead of
        // funneling everything onto a single TCP (where TCP-level HoL
        // serializes interleaved DATA frames). Tunable via env so we can
        // A/B without rebuilding.
        if (const char* s = std::getenv("NG_CURL_MAX_HOST_CONNS")) {
            curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS,
                              static_cast<long>(std::atol(s)));
        }

        while (!stop.load(std::memory_order_acquire)) {
            // Drain whatever's in the queue. We *don't* condvar-wait
            // here when the queue is empty: instead we let
            // curl_multi_poll block (with curl_multi_wakeup as the
            // cross-thread nudge), so we don't bounce between two
            // sleep primitives and add latency. submission path
            // calls curl_multi_wakeup → poll returns → we drain.
            std::deque<std::unique_ptr<Pending>> pulled;
            {
                std::lock_guard<std::mutex> lock(mu);
                pulled.swap(queue);
            }
            while (!pulled.empty()) {
                auto p = std::move(pulled.front()); pulled.pop_front();
                setup_and_add(std::move(p));
            }

            int still_running = 0;
            CURLMcode rc = curl_multi_perform(multi, &still_running);
            if (rc != CURLM_OK) break;

            // Long-ish poll — woken immediately by curl_multi_wakeup
            // from the submission path or by socket activity.
            int numfds = 0;
            curl_multi_poll(multi, nullptr, 0, 500, &numfds);

            // Drain completion messages.
            int msgs_left = 0;
            CURLMsg* msg = nullptr;
            while ((msg = curl_multi_info_read(multi, &msgs_left))) {
                if (msg->msg != CURLMSG_DONE) continue;
                CURL* easy = msg->easy_handle;
                CURLcode code = msg->data.result;
                finish(easy, code);
            }
        }

        // Drain remaining handlers with an error so awaiters wake up.
        // BOTH queues need draining — `active` for already-on-multi work,
        // and `queue` for submissions that arrived between our last
        // queue-drain and the stop check. Without this, a coroutine that
        // posts a request right before pool dtor would await forever
        // because its handler is sitting in `queue` with no worker to run it.
        std::deque<std::unique_ptr<Pending>> stranded;
        {
            std::lock_guard<std::mutex> lock(mu);
            stranded.swap(queue);
        }
        for (auto& p : stranded) {
            auto on_done   = std::move(p->on_done);
            auto caller_ex = std::move(p->caller_ex);
            asio::post(caller_ex, [on_done = std::move(on_done)]() mutable {
                on_done(HttpResponse{},
                        std::make_exception_ptr(std::runtime_error(
                            "CurlH2Pool destroyed before request was started")));
            });
        }
        for (auto& p : active) {
            curl_multi_remove_handle(multi, p->easy);
            // Move the handler off so it doesn't run under our lock.
            auto on_done   = std::move(p->on_done);
            auto caller_ex = std::move(p->caller_ex);
            asio::post(caller_ex, [on_done = std::move(on_done)]() mutable {
                on_done(HttpResponse{},
                        std::make_exception_ptr(std::runtime_error(
                            "CurlH2Pool destroyed before request completed")));
            });
            curl_slist_free_all(p->curl_hdrs);
            curl_easy_cleanup(p->easy);
        }
        active.clear();
    }

    void setup_and_add(std::unique_ptr<Pending> p) {
        p->easy = curl_easy_init();
        if (!p->easy) {
            auto on_done   = std::move(p->on_done);
            auto caller_ex = std::move(p->caller_ex);
            asio::post(caller_ex, [on_done = std::move(on_done)]() mutable {
                on_done(HttpResponse{},
                        std::make_exception_ptr(std::runtime_error(
                            "curl_easy_init failed")));
            });
            return;
        }

        p->curl_hdrs = curl_slist_append(nullptr, "Expect:");  // disable 100-continue
        for (const auto& [k, v] : p->headers) {
            std::string line = k + ": " + v;
            p->curl_hdrs = curl_slist_append(p->curl_hdrs, line.c_str());
        }

        curl_easy_setopt(p->easy, CURLOPT_URL,            p->url.c_str());
        curl_easy_setopt(p->easy, CURLOPT_HTTPHEADER,     p->curl_hdrs);
        curl_easy_setopt(p->easy, CURLOPT_POST,           1L);
        curl_easy_setopt(p->easy, CURLOPT_POSTFIELDS,     p->body.c_str());
        curl_easy_setopt(p->easy, CURLOPT_POSTFIELDSIZE,  static_cast<long>(p->body.size()));
        curl_easy_setopt(p->easy, CURLOPT_WRITEFUNCTION,  write_body_cb);
        curl_easy_setopt(p->easy, CURLOPT_WRITEDATA,      p.get());
        curl_easy_setopt(p->easy, CURLOPT_HEADERFUNCTION, header_line_cb);
        curl_easy_setopt(p->easy, CURLOPT_HEADERDATA,     p.get());
        curl_easy_setopt(p->easy, CURLOPT_HTTP_VERSION,   CURL_HTTP_VERSION_2TLS);
        // DIAG: PIPEWAIT=1 makes a request wait for an in-flight TLS handshake
        // to complete so it can multiplex onto the same conn. PIPEWAIT=0 lets
        // libcurl open a fresh conn instead of waiting. Tunable via env.
        long pipewait = 1L;
        if (const char* s = std::getenv("NG_CURL_PIPEWAIT")) pipewait = std::atol(s);
        curl_easy_setopt(p->easy, CURLOPT_PIPEWAIT,       pipewait);
        if (p->timeout_seconds > 0) {
            curl_easy_setopt(p->easy, CURLOPT_TIMEOUT, p->timeout_seconds);
        }
        curl_easy_setopt(p->easy, CURLOPT_PRIVATE, p.get());

        Pending* raw = p.get();
        active.push_back(std::move(p));
        curl_multi_add_handle(multi, raw->easy);
    }

    void finish(CURL* easy, CURLcode code) {
        // Locate the Pending by easy handle.
        auto it = std::find_if(active.begin(), active.end(),
            [easy](const std::unique_ptr<Pending>& p) {
                return p && p->easy == easy;
            });
        if (it == active.end()) return;
        std::unique_ptr<Pending> p = std::move(*it);
        active.erase(it);

        curl_multi_remove_handle(multi, easy);

        HttpResponse r;
        std::exception_ptr err;
        if (code == CURLE_OK) {
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &p->status);
            r.status  = static_cast<int>(p->status);
            r.headers = std::move(p->resp_headers);
            r.body    = std::move(p->resp_body);
            // RateLimitedProvider + the SchemaProvider 429 path read
            // `retry_after` and `location` directly off HttpResponse
            // (not via get_header) — populate those here so behaviour
            // matches the HTTP/1.1 free-function path.
            for (const auto& [k, v] : r.headers) {
                std::string lk; lk.reserve(k.size());
                for (char c : k) lk.push_back(static_cast<char>(std::tolower(
                    static_cast<unsigned char>(c))));
                if (lk == "retry-after" && r.retry_after.empty()) {
                    r.retry_after = v;
                } else if (lk == "location" && r.location.empty()) {
                    r.location = v;
                }
            }
        } else {
            err = std::make_exception_ptr(std::runtime_error(
                std::string("libcurl: ") + curl_easy_strerror(code)));
        }

        auto on_done   = std::move(p->on_done);
        auto caller_ex = std::move(p->caller_ex);
        asio::post(caller_ex,
            [r = std::move(r), err = std::move(err),
             on_done = std::move(on_done)]() mutable {
                on_done(std::move(r), std::move(err));
            });

        curl_slist_free_all(p->curl_hdrs);
        curl_easy_cleanup(easy);
    }
};

CurlH2Pool::CurlH2Pool() : impl_(std::make_unique<Impl>()) {
    static std::once_flag global_init_once;
    std::call_once(global_init_once, []{ curl_global_init(CURL_GLOBAL_DEFAULT); });
    impl_->multi = curl_multi_init();
    if (!impl_->multi) throw std::runtime_error("curl_multi_init failed");
    impl_->worker = std::thread([this]{ impl_->worker_loop(); });
}

CurlH2Pool::~CurlH2Pool() {
    impl_->stop.store(true, std::memory_order_release);
    if (impl_->multi) curl_multi_wakeup(impl_->multi);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->multi) curl_multi_cleanup(impl_->multi);
}

asio::awaitable<HttpResponse> CurlH2Pool::async_post(
    std::string url,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    RequestOptions opts) {

    auto ex = co_await asio::this_coro::executor;

    co_return co_await asio::async_initiate<
        decltype(asio::use_awaitable), void(HttpResponse)>(
        [this, ex,
         url = std::move(url),
         body = std::move(body),
         headers = std::move(headers),
         opts](auto handler) mutable {
            // Wrap the asio handler so the worker can call it without
            // touching asio types at the C/lock-protected layer.
            // Exceptions are tunneled out via the inner shared_ptr +
            // post-scheduled re-throw.
            auto p = std::make_unique<Pending>();
            p->url     = std::move(url);
            p->body    = std::move(body);
            p->headers = std::move(headers);
            p->timeout_seconds = (opts.timeout.count() > 0)
                ? std::chrono::duration_cast<std::chrono::seconds>(
                      opts.timeout).count()
                : 0;
            p->caller_ex = ex;
            // asio completion handlers are move-only; wrap into a
            // shared_ptr so std::function (used in Pending::on_done)
            // can hold it.
            auto h_sp = std::make_shared<std::decay_t<decltype(handler)>>(
                std::move(handler));
            p->on_done = [h_sp](HttpResponse r, std::exception_ptr err) {
                if (err) {
                    try { std::rethrow_exception(err); }
                    catch (const std::exception& e) {
                        HttpResponse e_resp;
                        e_resp.status = 0;
                        e_resp.body   = std::string("CurlH2Pool error: ") + e.what();
                        std::move(*h_sp)(std::move(e_resp));
                        return;
                    }
                }
                std::move(*h_sp)(std::move(r));
            };

            {
                std::lock_guard<std::mutex> lock(impl_->mu);
                impl_->queue.push_back(std::move(p));
            }
            // Wake the worker out of its curl_multi_poll immediately
            // — the submission must not wait the poll-timeout window
            // (was 100ms; even after raising it to 500ms, latency-
            // sensitive callers depend on this nudge).
            curl_multi_wakeup(impl_->multi);
        },
        asio::use_awaitable);
}

} // namespace neograph::async

#endif  // NEOGRAPH_HAVE_LIBCURL
