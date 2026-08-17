#pragma once
#ifdef HAS_BINANCE

#include "../../execution/fill_transport.h"
#include "binance_parser.h"
#include "binance_rest_client.h"
#include "providers/bounded_ws_open.h"
#include "providers/recovery_payload.h"
#include "providers/thread_safe_callback.h"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>

#include <openssl/ssl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

struct binance_keepalive_policy
{
    std::chrono::seconds interval     = std::chrono::seconds(30 * 60);
    std::chrono::seconds retry_delay  = std::chrono::seconds(15);
    int                  max_retries  = 3;
};

namespace binance_keepalive_detail {

inline std::string authoritative_listen_key(std::string_view body)
{
    if (!provider_recovery::is_authoritative_object(body)) return {};
    std::string_view key;
    if (!provider_recovery::top_level_plain_string(body, "listenKey", key)
        || key.empty())
        return {};
    return std::string(key);
}

struct ka_response
{
    int status = 0;
};

using put_fn  = std::function<ka_response(const std::string& listen_key)>;
// POST must write the new listenKey into out_key on success.
using post_fn = std::function<ka_response(std::string& out_key)>;

struct tick_result
{
    enum class kind { ok, rotated, error, stopped };
    kind k = kind::ok;
    std::string new_key;  // set when k == rotated
    std::string note;
};

// PUT up to max_retries; on total failure, rotate via POST. wait_fn sleeps
// up to `delay`, returning true if stop was observed during the wait.
template <typename WaitFn>
inline tick_result keepalive_tick(const binance_keepalive_policy& pol,
                                  const std::string& current_key,
                                  const put_fn&  put_call,
                                  const post_fn& post_call,
                                  std::atomic<bool>& stop,
                                  WaitFn wait_fn)
{
    tick_result r;
    for (int attempt = 1; attempt <= pol.max_retries; ++attempt)
    {
        if (stop.load()) { r.k = tick_result::kind::stopped; return r; }
        auto resp = put_call ? put_call(current_key) : ka_response{500};
        if (resp.status >= 200 && resp.status < 300)
        {
            r.k = tick_result::kind::ok;
            r.note = "listenKey refreshed";
            return r;
        }
        if (attempt < pol.max_retries)
        {
            if (wait_fn(pol.retry_delay, stop))
            {
                r.k = tick_result::kind::stopped;
                return r;
            }
        }
    }

    if (stop.load()) { r.k = tick_result::kind::stopped; return r; }

    std::string rotated_key;
    auto resp = post_call ? post_call(rotated_key) : ka_response{500};
    if (resp.status >= 200 && resp.status < 300 && !rotated_key.empty())
    {
        r.k = tick_result::kind::rotated;
        r.new_key = std::move(rotated_key);
        r.note = "listenKey rotated after repeated keepalive failures";
        return r;
    }

    r.k = tick_result::kind::error;
    r.note = "listenKey keepalive failed and rotation failed";
    return r;
}

}

class BinanceUserDataTransport : public IFillTransport
{
public:
    enum class run_result { stopped, network_error, handshake_error };
    enum class next_step  { retry_after, give_up, stop };
    using create_listen_key_fn =
        std::function<BinanceRestClient::response()>;
    using delete_listen_key_fn =
        std::function<void(const std::string&)>;
    using run_once_fn = std::function<run_result(std::atomic<bool>& stop)>;
    using keepalive_once_fn =
        std::function<binance_keepalive_detail::tick_result()>;

    BinanceUserDataTransport(std::shared_ptr<BinanceRestClient> rest,
                             std::string ws_host = "stream.binance.com",
                             std::string ws_port = "9443",
                             binance_keepalive_policy policy = {},
                             std::string listen_key_path
                                 = "/api/v3/userDataStream")
        : rest_(std::move(rest))
        , ws_host_(std::move(ws_host))
        , ws_port_(std::move(ws_port))
        , listen_key_path_(std::move(listen_key_path))
        , keepalive_policy_(policy)
        , ws_ctx_(boost::asio::ssl::context::tlsv12_client)
    {
        ws_ctx_.set_default_verify_paths();
        ws_ctx_.set_verify_mode(boost::asio::ssl::verify_peer);

        // TLS session resumption across reconnects: 24h disconnect cap +
        // monthly testnet wipes mean this socket genuinely flaps. Cache one
        // most-recent ticket; OpenSSL falls back to a full handshake if the
        // server rejects it.
        SSL_CTX* raw = ws_ctx_.native_handle();
        SSL_CTX_set_session_cache_mode(
            raw, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
        SSL_CTX_set_ex_data(raw, ex_data_index(), this);
        SSL_CTX_sess_set_new_cb(raw, &BinanceUserDataTransport::on_new_session);
        if (rest_)
        {
            const auto path = listen_key_path_;
            create_listen_key_ = [client = rest_, path] {
                return client->post_unsigned(path);
            };
            delete_listen_key_ = [client = rest_, path](const std::string& key) {
                (void)client->safety_del(
                    path, "listenKey=" + binance::url_encode(key),
                    std::chrono::seconds(1));
            };
        }
    }

    // Deterministic seam for readiness/teardown tests. Production always uses
    // the REST-backed constructor above.
    BinanceUserDataTransport(create_listen_key_fn create_key,
                             delete_listen_key_fn delete_key,
                             run_once_fn run_once,
                             keepalive_once_fn keepalive_once = {},
                             binance_keepalive_policy policy = {},
                             bool override_reports_ready = false)
        : BinanceUserDataTransport(nullptr, "localhost", "1")
    {
        create_listen_key_ = std::move(create_key);
        delete_listen_key_ = std::move(delete_key);
        run_once_override_ = std::move(run_once);
        keepalive_once_override_ = std::move(keepalive_once);
        keepalive_policy_ = policy;
        override_reports_ready_ = override_reports_ready;
    }

    ~BinanceUserDataTransport() override
    {
        close();
        SSL_SESSION* sess = cached_session_.exchange(
            nullptr, std::memory_order_acq_rel);
        if (sess) SSL_SESSION_free(sess);
    }

    BinanceUserDataTransport(const BinanceUserDataTransport&) = delete;
    BinanceUserDataTransport& operator=(const BinanceUserDataTransport&) = delete;
    BinanceUserDataTransport(BinanceUserDataTransport&&) = delete;
    BinanceUserDataTransport& operator=(BinanceUserDataTransport&&) = delete;

    bool open() override
    {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            if (state_ == lifecycle::open || state_ == lifecycle::connecting)
                return true;
        }

        set_state(lifecycle::connecting, "creating listenKey");

        if (!create_listen_key_)
        {
            set_state(lifecycle::error, "no REST client");
            return false;
        }

        auto resp = create_listen_key_();
        const auto created_key =
            binance_keepalive_detail::authoritative_listen_key(resp.body);
        if (resp.status < 200 || resp.status >= 300 || created_key.empty())
        {
            set_state(lifecycle::error,
                      "listenKey create HTTP " + std::to_string(resp.status));
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(listen_key_mu_);
            listen_key_ = created_key;
        }
        if (current_listen_key().empty())
        {
            set_state(lifecycle::error, "listenKey missing in response");
            return false;
        }

        stop_flag_ = false;
        ever_open_.store(false, std::memory_order_release);
        reader_    = std::thread([this] { run(); });
        keepalive_ = std::thread([this] { keepalive_loop(); });

        {
            std::unique_lock<std::mutex> lk(state_mu_);
            open_cv_.wait_for(lk, std::chrono::seconds(5), [this] {
                return state_ == lifecycle::open
                    || state_ == lifecycle::error
                    || stop_flag_.load(std::memory_order_acquire);
            });
            if (state_ == lifecycle::open) return true;
        }

        close();
        return false;
    }

    void close() override
    {
        stop_flag_ = true;
        stop_flag_.notify_all();
        cv_.notify_all();

        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_)
            {
                try
                {
                    boost::beast::error_code ec;
                    auto& lowest = boost::beast::get_lowest_layer(*ws_);
                    lowest.cancel(ec);
                    lowest.close(ec);
                }
                catch (...) {}
            }
        }

        if (reader_.joinable())    reader_.join();
        if (keepalive_.joinable()) keepalive_.join();

        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_)
            {
                try
                {
                    boost::beast::error_code ec;
                    ws_->close(boost::beast::websocket::close_code::normal, ec);
                }
                catch (...) {}
            }
            ws_.reset();
            ioc_.restart();
        }

        std::string key_to_delete;
        {
            std::lock_guard<std::mutex> lk(listen_key_mu_);
            key_to_delete = listen_key_;
            listen_key_.clear();
        }
        if (delete_listen_key_ && !key_to_delete.empty())
        {
            try { delete_listen_key_(key_to_delete); }
            catch (...) {}
        }

        set_state(lifecycle::closed, "closed");
    }

    lifecycle state() const override
    {
        std::lock_guard<std::mutex> lk(state_mu_);
        return state_;
    }

    void set_on_message(message_cb cb) override { message_cb_ = std::move(cb); }
    void set_on_status(status_cb cb)   override { status_cb_  = std::move(cb); }

    // Engine wires this in live mode. When set, a fatal user-data
    // disconnect (network_error / handshake_error past retry budget)
    // calls back here and the run() loop returns immediately - bypasses
    // the up-to-10-attempt reconnect schedule. Default unset preserves
    // the original behaviour for unit tests / non-live paths.
    void set_fatal_disconnect_callback(
        std::function<void(std::string_view reason)> cb) override
    {
        fatal_cb_.store(std::move(cb));
    }

    std::string listen_key() const
    {
        std::lock_guard<std::mutex> lk(listen_key_mu_);
        return listen_key_;
    }

    struct reconnect_state
    {
        int attempt = 0;
        long long last_open_ms = 0;
        bool stop = false;
    };

    static next_step decide_next(reconnect_state s,
                                 run_result last,
                                 int max_attempts,
                                 long long now_ms,
                                 long long reset_threshold_ms)
    {
        if (s.stop || last == run_result::stopped) return next_step::stop;

        if (s.last_open_ms > 0 &&
            (now_ms - s.last_open_ms) > reset_threshold_ms)
            return next_step::retry_after;

        if (s.attempt + 1 >= max_attempts) return next_step::give_up;
        return next_step::retry_after;
    }

private:
    static int ex_data_index()
    {
        static const int idx = SSL_CTX_get_ex_new_index(
            0, const_cast<char*>("BinanceUserDataTransport::this"),
            nullptr, nullptr, nullptr);
        return idx;
    }

    static int on_new_session(SSL* ssl, SSL_SESSION* session)
    {
        SSL_CTX* ctx = SSL_get_SSL_CTX(ssl);
        auto* self = static_cast<BinanceUserDataTransport*>(
            SSL_CTX_get_ex_data(ctx, ex_data_index()));
        if (!self) return 0;
        SSL_SESSION* old = self->cached_session_.exchange(
            session, std::memory_order_acq_rel);
        if (old) SSL_SESSION_free(old);
        return 1;
    }

    std::string current_listen_key() const
    {
        std::lock_guard<std::mutex> lk(listen_key_mu_);
        return listen_key_;
    }

    void set_state(lifecycle s, std::string note)
    {
        {
            std::lock_guard<std::mutex> lk(state_mu_);
            state_ = s;
            if (s == lifecycle::open || s == lifecycle::error)
                open_cv_.notify_all();
        }
        if (status_cb_)
        {
            try { status_cb_(s, note); }
            catch (...) {}
        }
    }

    run_result run_once()
    {
        namespace beast = boost::beast;
        namespace websocket = beast::websocket;
        namespace net = boost::asio;
        namespace ssl = net::ssl;
        using tcp = net::ip::tcp;

        bool reached_open = false;
        try
        {
            constexpr auto connect_deadline = std::chrono::seconds(3);
            {
                std::lock_guard<std::mutex> lk(ws_mu_);
                ws_ = std::make_unique<
                    websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc_, ws_ctx_);
            }
            std::string target = "/ws/" + current_listen_key();
            const bool ws_ready = provider_ws::open_tls_websocket(
                ioc_, *ws_, ws_host_, ws_port_, target, connect_deadline,
                [&](auto& socket) {
                    auto& lowest = beast::get_lowest_layer(socket);
                    const int yes = 1;
                    const int idle = 1, intvl = 1, cnt = 2;
                    const int fd = lowest.native_handle();
                    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &yes, sizeof(yes));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
                    ::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
                    if (SSL_SESSION* sess =
                            cached_session_.load(std::memory_order_acquire))
                        SSL_set_session(socket.next_layer().native_handle(), sess);
                },
                [&](auto& socket) {
                    websocket::stream_base::timeout opt;
                    opt.handshake_timeout = std::chrono::seconds(3);
                    opt.idle_timeout = std::chrono::milliseconds(1500);
                    opt.keep_alive_pings = true;
                    socket.set_option(opt);
                    socket.set_option(websocket::stream_base::decorator(
                        [](websocket::request_type& req) {
                            req.set(boost::beast::http::field::user_agent,
                                    "TrueTest/1.0");
                        }));
                });
            if (!ws_ready || stop_flag_.load(std::memory_order_acquire))
                return stop_flag_.load(std::memory_order_acquire)
                    ? run_result::stopped : run_result::handshake_error;

            reached_open = true;
            ever_open_.store(true, std::memory_order_release);
            set_state(lifecycle::open, "user-data stream open");

            beast::flat_buffer buf;
            while (!stop_flag_.load())
            {
                buf.consume(buf.size());
                beast::error_code read_ec;
                if (ioc_.stopped()) ioc_.restart();
                ws_->async_read(buf, [&](beast::error_code ec, std::size_t) {
                    read_ec = ec;
                });
                ioc_.run();
                if (read_ec)
                    throw beast::system_error(read_ec);

                if (stop_flag_.load()) break;

                auto data = buf.data();
                std::string_view view(
                    static_cast<const char*>(data.data()), data.size());

                if (message_cb_) message_cb_(view);
            }
            return run_result::stopped;
        }
        catch (const boost::beast::system_error& se)
        {
            if (stop_flag_.load()) return run_result::stopped;
            return reached_open ? run_result::network_error
                                : run_result::handshake_error;
        }
        catch (const std::exception&)
        {
            if (stop_flag_.load()) return run_result::stopped;
            return reached_open ? run_result::network_error
                                : run_result::handshake_error;
        }
    }

    void run()
    {
        if (stop_flag_.load()) return;
        if (run_once_override_ && override_reports_ready_)
        {
            ever_open_.store(true, std::memory_order_release);
            set_state(lifecycle::open, "test user-data stream open");
        }
        auto r = run_once_override_ ? run_once_override_(stop_flag_)
                                    : run_once();
        if (r == run_result::stopped) return;
        if (!ever_open_.load(std::memory_order_acquire))
        {
            set_state(lifecycle::error,
                      "initial user-data stream handshake failed");
            return;
        }

        const char* what =
            (r == run_result::network_error) ? "network error"
                                             : "handshake error";
        char fatal_reason[160];
        std::snprintf(fatal_reason, sizeof(fatal_reason),
                      "binance user-data WS lost: %s", what);
        stop_flag_.store(true);
        stop_flag_.notify_all();
        set_state(lifecycle::error, fatal_reason);
        fatal_cb_.publish(fatal_reason);
    }

    void keepalive_loop()
    {
        while (!stop_flag_.load())
        {
            {
                std::unique_lock<std::mutex> lk(cv_mu_);
                cv_.wait_for(lk, keepalive_policy_.interval,
                             [this] { return stop_flag_.load(); });
            }
            if (stop_flag_.load()) break;

            binance_keepalive_detail::tick_result out;
            try
            {
                if (keepalive_once_override_)
                {
                    out = keepalive_once_override_();
                }
                else
                {
                    if (!rest_) continue;
                    std::string current;
                    {
                        std::lock_guard<std::mutex> lk(listen_key_mu_);
                        current = listen_key_;
                    }
                    if (current.empty()) continue;

                    using binance_keepalive_detail::ka_response;
                    auto put_call = [this](const std::string& key) {
                        auto r = rest_->put_unsigned(
                            listen_key_path_,
                            "listenKey=" + binance::url_encode(key));
                        return ka_response{r.status};
                    };
                    auto post_call = [this](std::string& out_key) {
                        auto r = rest_->post_unsigned(listen_key_path_);
                        if (r.status >= 200 && r.status < 300)
                        {
                            out_key = binance_keepalive_detail::
                                authoritative_listen_key(r.body);
                        }
                        return ka_response{r.status};
                    };
                    auto wait_fn = [this](std::chrono::seconds delay,
                                          std::atomic<bool>& stop) -> bool {
                        std::unique_lock<std::mutex> lk(cv_mu_);
                        return cv_.wait_for(
                            lk, delay, [&stop] { return stop.load(); });
                    };
                    out = binance_keepalive_detail::keepalive_tick(
                        keepalive_policy_, current, put_call, post_call,
                        stop_flag_, wait_fn);
                }
            }
            catch (...)
            {
                out.k = binance_keepalive_detail::tick_result::kind::error;
                out.note = "listenKey keepalive threw";
            }

            using K = binance_keepalive_detail::tick_result::kind;
            if (out.k == K::ok)
            {
                set_state(lifecycle::open, "listenKey refreshed");
            }
            else if (out.k == K::rotated)
            {
                {
                    std::lock_guard<std::mutex> lk(listen_key_mu_);
                    listen_key_ = out.new_key;
                }
                set_state(lifecycle::degraded, out.note);
            }
            else if (out.k == K::error)
            {
                set_state(lifecycle::error, out.note);
                stop_flag_.store(true, std::memory_order_release);
                stop_flag_.notify_all();
                cv_.notify_all();
                fatal_cb_.publish(out.note.empty()
                    ? std::string_view{"binance listenKey keepalive failed"}
                    : std::string_view{out.note});
                interrupt_websocket();
                break;
            }
        }
    }

    void interrupt_websocket() noexcept
    {
        try
        {
            std::lock_guard<std::mutex> lk(ws_mu_);
            if (ws_)
            {
                boost::beast::error_code ec;
                auto& lowest = boost::beast::get_lowest_layer(*ws_);
                lowest.cancel(ec);
                lowest.close(ec);
            }
            ioc_.stop();
        }
        catch (...) {}
    }

    std::shared_ptr<BinanceRestClient> rest_;
    std::string ws_host_;
    std::string ws_port_;
    std::string listen_key_path_;
    mutable std::mutex listen_key_mu_;
    std::string listen_key_;
    binance_keepalive_policy keepalive_policy_;
    boost::asio::io_context ioc_;
    boost::asio::ssl::context ws_ctx_;
    std::atomic<SSL_SESSION*> cached_session_{nullptr};

    std::unique_ptr<boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>> ws_;
    std::mutex ws_mu_;

    message_cb message_cb_;
    status_cb  status_cb_;
    LatchedFailureCallback fatal_cb_;

    std::thread reader_;
    std::thread keepalive_;
    std::atomic<bool> stop_flag_{false};
    std::atomic<bool> ever_open_{false};

    create_listen_key_fn create_listen_key_;
    delete_listen_key_fn delete_listen_key_;
    run_once_fn run_once_override_;
    keepalive_once_fn keepalive_once_override_;
    bool override_reports_ready_ = false;

    std::mutex cv_mu_;
    std::condition_variable cv_;

    mutable std::mutex state_mu_;
    std::condition_variable open_cv_;
    lifecycle state_ = lifecycle::closed;
};

#endif // HAS_BINANCE
