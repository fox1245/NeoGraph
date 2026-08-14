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
#include <neograph/async/endpoint.h>
#include "http_exchange_detail.h"

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

#include <asio/associated_cancellation_slot.hpp>
#include <asio/post.hpp>
#include <asio/this_coro.hpp>
#include <asio/use_awaitable.hpp>
#include <asio/async_result.hpp>
#include <asio/error.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace neograph::async {

namespace {

struct RequestControl {
    std::atomic<bool> cancelled{false};
};

// Cancellation callbacks run on the caller's executor while curl's multi
// handle belongs exclusively to the worker. Serialize wakeup against pool
// teardown so a late callback never reaches a cleaned-up CURLM handle.
struct MultiWakeup {
    std::mutex mutex;
    CURLM* multi = nullptr;

    void wake() noexcept {
        std::lock_guard lock(mutex);
        if (multi) (void)curl_multi_wakeup(multi);
    }

    void clear() noexcept {
        std::lock_guard lock(mutex);
        multi = nullptr;
    }
};

// Per-request state owned by the worker until completion. The handler keeps
// Asio's error-code completion convention while preserving the pool's legacy
// status-0 response for ordinary libcurl transport failures.
struct Pending {
    std::string url;
    std::string body;
    std::vector<std::pair<std::string, std::string>> headers;
    RequestOptions opts;
    std::optional<detail::HttpTarget> current_target;
    int redirects_followed = 0;

    asio::any_io_executor caller_ex;
    std::function<void(asio::error_code, HttpResponse, std::exception_ptr)> on_done;
    std::shared_ptr<RequestControl> control = std::make_shared<RequestControl>();

    // Filled by the worker as the request runs.
    std::string resp_body;
    std::vector<std::pair<std::string, std::string>> resp_headers;
    long status = 0;
    std::string error;
    std::size_t resp_header_bytes = 0;
    std::optional<std::size_t> content_length;
    bool limit_error = false;
    bool callback_error = false;

    CURL*              easy        = nullptr;
    struct curl_slist* curl_hdrs   = nullptr;
};

bool header_name_equals(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size()) return false;
    return std::equal(left.begin(), left.end(), right.begin(),
        [](unsigned char a, unsigned char b) {
            return std::tolower(a) == std::tolower(b);
        });
}

std::size_t write_body_cb(char* p, std::size_t s, std::size_t n, void* up) {
    auto* st = static_cast<Pending*>(up);
    if (s != 0 && n > std::numeric_limits<std::size_t>::max() / s) {
        st->limit_error = true;
        return 0;
    }
    const std::size_t bytes = s * n;
    if (detail::exceeds_limit(bytes, st->opts.max_response_chunk_bytes) ||
        bytes > std::numeric_limits<std::size_t>::max() - st->resp_body.size() ||
        bytes > st->resp_body.max_size() -
            std::min(st->resp_body.size(), st->resp_body.max_size()) ||
        (st->opts.max_response_body_bytes != 0 &&
         bytes > st->opts.max_response_body_bytes -
             std::min(st->resp_body.size(), st->opts.max_response_body_bytes))) {
        st->limit_error = true;
        return 0;
    }
    try {
        st->resp_body.append(p, bytes);
    } catch (...) {
        st->callback_error = true;
        return 0;
    }
    return bytes;
}

// libcurl emits each HTTP header as one line including CRLF. Skip
// the status line ("HTTP/...") and any trailing blank line.
std::size_t header_line_cb(char* p, std::size_t s, std::size_t n, void* up) {
    auto* st = static_cast<Pending*>(up);
    if (s != 0 && n > std::numeric_limits<std::size_t>::max() / s) {
        st->limit_error = true;
        return 0;
    }
    const std::size_t bytes = s * n;
    if (bytes > std::numeric_limits<std::size_t>::max() - st->resp_header_bytes ||
        (st->opts.max_response_header_bytes != 0 &&
         bytes > st->opts.max_response_header_bytes -
             std::min(st->resp_header_bytes, st->opts.max_response_header_bytes))) {
        st->limit_error = true;
        return 0;
    }
    st->resp_header_bytes += bytes;

    std::string_view line(p, bytes);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
        line.remove_suffix(1);
    }
    if (line.empty()) return bytes;
    if (line.substr(0, 5) == "HTTP/") {
        st->content_length.reset();
        return bytes;
    }
    auto colon = line.find(':');
    if (colon == std::string_view::npos) return bytes;
    const std::string_view name_view = line.substr(0, colon);
    std::string_view raw_value = line.substr(colon + 1);
    while (!raw_value.empty() &&
           (raw_value.front() == ' ' || raw_value.front() == '\t')) {
        raw_value.remove_prefix(1);
    }
    while (!raw_value.empty() &&
           (raw_value.back() == ' ' || raw_value.back() == '\t')) {
        raw_value.remove_suffix(1);
    }
    if (header_name_equals(name_view, "content-length")) {
        std::size_t parsed = 0;
        const auto [end, ec] = std::from_chars(
            raw_value.data(), raw_value.data() + raw_value.size(), parsed, 10);
        if (ec == std::errc::result_out_of_range ||
            detail::exceeds_limit(parsed, st->opts.max_response_body_bytes) ||
            parsed > st->resp_body.max_size()) {
            st->limit_error = true;
            return 0;
        }
        if (raw_value.empty() || ec != std::errc{} ||
            end != raw_value.data() + raw_value.size() ||
            (st->content_length && *st->content_length != parsed)) {
            st->callback_error = true;
            return 0;
        }
        st->content_length = parsed;
    }
    try {
        st->resp_headers.emplace_back(name_view, raw_value);
    } catch (...) {
        st->callback_error = true;
        return 0;
    }
    return bytes;
}

int xferinfo_cb(void* up,
                curl_off_t,
                curl_off_t,
                curl_off_t,
                curl_off_t) {
    auto* st = static_cast<Pending*>(up);
    return st->control->cancelled.load(std::memory_order_acquire) ? 1 : 0;
}

constexpr bool is_redirect_status(int status) noexcept {
    return status == 301 || status == 302 || status == 303 ||
           status == 307 || status == 308;
}

std::string_view first_header(
    const std::vector<std::pair<std::string, std::string>>& headers,
    std::string_view wanted) {
    for (const auto& [name, value] : headers) {
        if (header_name_equals(name, wanted)) return value;
    }
    return {};
}

void reset_response(Pending& pending) {
    pending.resp_body.clear();
    pending.resp_headers.clear();
    pending.status = 0;
    pending.error.clear();
    pending.resp_header_bytes = 0;
    pending.content_length.reset();
    pending.limit_error = false;
    pending.callback_error = false;
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
    std::shared_ptr<MultiWakeup>          wakeup = std::make_shared<MultiWakeup>();

    void complete(std::unique_ptr<Pending> p,
                  asio::error_code ec = {},
                  HttpResponse response = {},
                  std::exception_ptr err = {}) {
        auto on_done = std::move(p->on_done);
        auto caller_ex = std::move(p->caller_ex);
        asio::post(caller_ex,
            [ec, response = std::move(response), err = std::move(err),
             on_done = std::move(on_done)]() mutable {
                on_done(ec, std::move(response), std::move(err));
            });
    }

    void cancel_active() {
        std::vector<CURL*> cancelled;
        cancelled.reserve(active.size());
        for (const auto& p : active) {
            if (p->control->cancelled.load(std::memory_order_acquire)) {
                cancelled.push_back(p->easy);
            }
        }
        for (auto* easy : cancelled) {
            finish(easy, CURLE_ABORTED_BY_CALLBACK);
        }
    }

    void worker_loop() {
        // 1.62+ default; explicit so older runtimes also multiplex.
        curl_multi_setopt(multi, CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX);
        // DIAG: allow multiple H/2 connections per host (default 0 = 1).
        // Lets libcurl spread N parallel streams across M conns instead of
        // funneling everything onto a single TCP (where TCP-level HoL
        // serializes interleaved DATA frames). Tunable via env so we can
        // A/B without rebuilding.
        if (const char* s = std::getenv("NG_CURL_MAX_HOST_CONNS")) {
            // Validate before forwarding to libcurl. Negative or
            // absurdly large values pass straight through `std::atol`
            // and would either be reinterpreted as ULONG_MAX or
            // silently overflow libcurl's internal counters. Clamp
            // to a sensible engineering range.
            long v = std::atol(s);
            if (v >= 1 && v <= 4096) {
                curl_multi_setopt(multi, CURLMOPT_MAX_HOST_CONNECTIONS, v);
            }
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

            // A cancellation callback only sets a per-request flag and wakes
            // this worker. Removing the easy handle here keeps all curl_multi
            // mutation on its owner thread.
            cancel_active();

            int still_running = 0;
            CURLMcode rc = curl_multi_perform(multi, &still_running);
            if (rc != CURLM_OK) break;

            cancel_active();

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
            complete(std::move(p), asio::error::operation_aborted);
        }
        for (auto& p : active) {
            curl_multi_remove_handle(multi, p->easy);
            curl_slist_free_all(p->curl_hdrs);
            curl_easy_cleanup(p->easy);
            complete(std::move(p), asio::error::operation_aborted);
        }
        active.clear();
    }

    void setup_and_add(std::unique_ptr<Pending> p) {
        if (p->control->cancelled.load(std::memory_order_acquire)) {
            complete(std::move(p), asio::error::operation_aborted);
            return;
        }
        if (!p->current_target) {
            complete(std::move(p), {}, {}, std::make_exception_ptr(
                std::runtime_error("CurlH2Pool: invalid HTTP URL")));
            return;
        }
        p->easy = curl_easy_init();
        if (!p->easy) {
            complete(std::move(p), {}, {},
                std::make_exception_ptr(std::runtime_error("curl_easy_init failed")));
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
        curl_easy_setopt(p->easy, CURLOPT_POSTFIELDSIZE_LARGE,
                         static_cast<curl_off_t>(p->body.size()));
        curl_easy_setopt(p->easy, CURLOPT_WRITEFUNCTION,  write_body_cb);
        curl_easy_setopt(p->easy, CURLOPT_WRITEDATA,      p.get());
        curl_easy_setopt(p->easy, CURLOPT_HEADERFUNCTION, header_line_cb);
        curl_easy_setopt(p->easy, CURLOPT_HEADERDATA,     p.get());
        curl_easy_setopt(p->easy, CURLOPT_NOPROGRESS,      0L);
        curl_easy_setopt(p->easy, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
        curl_easy_setopt(p->easy, CURLOPT_XFERINFODATA,    p.get());
        curl_easy_setopt(p->easy, CURLOPT_HTTP_VERSION,   CURL_HTTP_VERSION_2TLS);
        if (!p->current_target->tls &&
            is_loopback_host(p->current_target->host)) {
            // The explicit plaintext exception is direct-loopback only. Do not
            // let HTTP_PROXY turn it into a credentialed remote cleartext hop.
            curl_easy_setopt(p->easy, CURLOPT_NOPROXY, "*");
        }
        // Redirects are replayed manually only after same-origin validation.
        curl_easy_setopt(p->easy, CURLOPT_FOLLOWLOCATION, 0L);
        // DIAG: PIPEWAIT=1 makes a request wait for an in-flight TLS handshake
        // to complete so it can multiplex onto the same conn. PIPEWAIT=0 lets
        // libcurl open a fresh conn instead of waiting. Tunable via env.
        long pipewait = 1L;
        if (const char* s = std::getenv("NG_CURL_PIPEWAIT")) {
            // PIPEWAIT is a boolean (0 or 1); reject anything else
            // rather than passing arbitrary integers to libcurl.
            long v = std::atol(s);
            if (v == 0 || v == 1) pipewait = v;
        }
        curl_easy_setopt(p->easy, CURLOPT_PIPEWAIT,       pipewait);
        if (p->opts.timeout.count() > 0) {
            const auto timeout = std::min<std::int64_t>(
                p->opts.timeout.count(), std::numeric_limits<long>::max());
            curl_easy_setopt(p->easy, CURLOPT_TIMEOUT_MS,
                             static_cast<long>(timeout));
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

        if (p->limit_error) {
            // The callback abort leaves response framing incomplete. Tell
            // libcurl not to return this connection to the multi cache.
            curl_easy_setopt(easy, CURLOPT_FORBID_REUSE, 1L);
        }
        curl_multi_remove_handle(multi, easy);

        const bool cancelled =
            p->control->cancelled.load(std::memory_order_acquire) ||
            code == CURLE_ABORTED_BY_CALLBACK;

        HttpResponse r;
        std::exception_ptr err;
        if (!cancelled && !p->limit_error && !p->callback_error &&
            code == CURLE_OK) {
            curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &p->status);

            const std::string_view location = first_header(
                p->resp_headers, "location");
            if (is_redirect_status(static_cast<int>(p->status)) &&
                p->opts.max_redirects > 0 &&
                p->redirects_followed < p->opts.max_redirects &&
                !location.empty()) {
                const auto next = detail::resolve_redirect_target(
                    *p->current_target, location);
                if (!next || (p->current_target->tls && !next->tls) ||
                    !detail::same_origin(*p->current_target, *next)) {
                    err = std::make_exception_ptr(std::runtime_error(
                        "CurlH2Pool: redirect rejected by same-origin policy"));
                } else {
                    p->current_target = *next;
                    p->url = detail::format_http_url(*next);
                    ++p->redirects_followed;
                    reset_response(*p);
                    curl_easy_setopt(easy, CURLOPT_URL, p->url.c_str());

                    Pending* raw = p.get();
                    active.push_back(std::move(p));
                    if (curl_multi_add_handle(multi, raw->easy) == CURLM_OK) {
                        return;
                    }
                    p = std::move(active.back());
                    active.pop_back();
                    err = std::make_exception_ptr(std::runtime_error(
                        "CurlH2Pool: redirect dispatch failed"));
                }
            }

            if (!err) {
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
            }
        } else if (!cancelled && !p->limit_error && p->callback_error) {
            err = std::make_exception_ptr(std::runtime_error(
                "CurlH2Pool: malformed response framing"));
        } else if (!cancelled && !p->limit_error &&
                   code != CURLE_OPERATION_TIMEDOUT) {
            err = std::make_exception_ptr(std::runtime_error(
                std::string("libcurl: ") + curl_easy_strerror(code)));
        }

        curl_slist_free_all(p->curl_hdrs);
        curl_easy_cleanup(easy);
        const asio::error_code ec = cancelled
            ? asio::error::operation_aborted
            : p->limit_error
                ? asio::error::message_size
            : code == CURLE_OPERATION_TIMEDOUT
                ? asio::error::timed_out
                : asio::error_code{};
        complete(std::move(p),
                 ec,
                 std::move(r), std::move(err));
    }
};

CurlH2Pool::CurlH2Pool() : impl_(std::make_unique<Impl>()) {
    static std::once_flag global_init_once;
    std::call_once(global_init_once, []{ curl_global_init(CURL_GLOBAL_DEFAULT); });
    impl_->multi = curl_multi_init();
    if (!impl_->multi) throw std::runtime_error("curl_multi_init failed");
    {
        std::lock_guard lock(impl_->wakeup->mutex);
        impl_->wakeup->multi = impl_->multi;
    }
    impl_->worker = std::thread([this]{ impl_->worker_loop(); });
}

CurlH2Pool::~CurlH2Pool() {
    impl_->stop.store(true, std::memory_order_release);
    impl_->wakeup->wake();
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->multi) {
        impl_->wakeup->clear();
        curl_multi_cleanup(impl_->multi);
    }
}

asio::awaitable<HttpResponse> CurlH2Pool::async_post(
    std::string url,
    std::string body,
    std::vector<std::pair<std::string, std::string>> headers,
    RequestOptions opts) {

    auto ex = co_await asio::this_coro::executor;

    co_return co_await asio::async_initiate<
        decltype(asio::use_awaitable), void(asio::error_code, HttpResponse)>(
        [this, ex,
         url = std::move(url),
         body = std::move(body),
         headers = std::move(headers),
         opts](auto handler) mutable {
            // Wrap the asio handler so the worker can call it without
            // touching asio types at the C/lock-protected layer.
            // Non-cancellation failures become the pool's historical
            // status-0 response; cancellation preserves Asio's
            // operation_aborted error for graph-level propagation.
            auto p = std::make_unique<Pending>();
            p->url     = std::move(url);
            p->body    = std::move(body);
            p->headers = std::move(headers);
            p->opts = opts;
            p->current_target = detail::parse_absolute_http_url(p->url);
            p->caller_ex = ex;
            auto slot = asio::get_associated_cancellation_slot(handler);
            // asio completion handlers are move-only; wrap into a
            // shared_ptr so std::function (used in Pending::on_done)
            // can hold it.
            auto h_sp = std::make_shared<std::decay_t<decltype(handler)>>(
                std::move(handler));
            p->on_done = [h_sp](asio::error_code ec,
                                HttpResponse r,
                                std::exception_ptr err) {
                if (ec) {
                    std::move(*h_sp)(ec, HttpResponse{});
                    return;
                }
                if (err) {
                    try { std::rethrow_exception(err); }
                    catch (const std::exception& e) {
                        HttpResponse e_resp;
                        e_resp.status = 0;
                        e_resp.body   = std::string("CurlH2Pool error: ") + e.what();
                        std::move(*h_sp)(asio::error_code{}, std::move(e_resp));
                        return;
                    }
                }
                std::move(*h_sp)(asio::error_code{}, std::move(r));
            };

            if (slot.is_connected()) {
                auto control = p->control;
                auto wakeup = impl_->wakeup;
                slot.assign([control = std::move(control),
                             wakeup = std::move(wakeup)](
                                asio::cancellation_type_t type) {
                    if (!type) return;
                    control->cancelled.store(true, std::memory_order_release);
                    wakeup->wake();
                });
            }

            {
                std::lock_guard<std::mutex> lock(impl_->mu);
                impl_->queue.push_back(std::move(p));
            }
            // Wake the worker out of its curl_multi_poll immediately
            // — the submission must not wait the poll-timeout window
            // (was 100ms; even after raising it to 500ms, latency-
            // sensitive callers depend on this nudge).
            impl_->wakeup->wake();
        },
        asio::use_awaitable);
}

} // namespace neograph::async

#endif  // NEOGRAPH_HAVE_LIBCURL
