#pragma once
#ifdef HAS_BINANCE

#include "../../execution/fill_transport.h"
#include "binance_parser.h"
#include "binance_rest_client.h"

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
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

struct binance_keepalive_policy
{
    std::chrono::seconds interval     = std::chrono::seconds(30 * 60);
    std::chrono::seconds retry_delay  = std::chrono::seconds(15);
    int                  max_retries  = 3;
};

namespace binance_keepalive_detail {

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

} // namespace binance_keepalive_detail

class BinanceUserDataTransport : public IFillTransport
{
public:
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

        if (!rest_)
        {
            set_state(lifecycle::error, "no REST client");
            return false;
        }

        auto resp = rest_->post_unsigned(listen_key_path_);
        if (resp.status < 200 || resp.status >= 300)
        {
            set_state(lifecycle::error,
                      "listenKey create HTTP " + std::to_string(resp.status));
            return false;
        }

        {
            std::lock_guard<std::mutex> lk(listen_key_mu_);
            listen_key_ = binance::extract_string(resp.body, "listenKey");
        }
        if (current_listen_key().empty())
        {
            set_state(lifecycle::error, "listenKey missing in response");
            return false;
        }

        stop_flag_ = false;
        reader_    = std::thread([this] { run(); });
        keepalive_ = std::thread([this] { keepalive_loop(); });
        return true;
    }

    void close() override
    {
        stop_flag_ = true;
        cv_.notify_all();

        if (ws_)
        {
            try
            {
                boost::beast::error_code ec;
                ws_->close(boost::beast::websocket::close_code::normal, ec);
            }
            catch (...) {}
        }

        if (reader_.joinable())    reader_.join();
        if (keepalive_.joinable()) keepalive_.join();

        std::string key_to_delete;
        {
            std::lock_guard<std::mutex> lk(listen_key_mu_);
            key_to_delete = listen_key_;
            listen_key_.clear();
        }
        if (rest_ && !key_to_delete.empty())
        {
            try
            {
                rest_->del(listen_key_path_,
                           "listenKey=" + key_to_delete);
            }
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

    std::string listen_key() const
    {
        std::lock_guard<std::mutex> lk(listen_key_mu_);
        return listen_key_;
    }

    enum class run_result { stopped, network_error, handshake_error };
    enum class next_step  { retry_after, give_up, stop };
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
        }
        if (status_cb_) status_cb_(s, note);
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
            net::io_context ioc;

            tcp::resolver resolver(ioc);
            auto results = resolver.resolve(ws_host_, ws_port_);

            ws_ = std::make_unique<
                websocket::stream<beast::ssl_stream<tcp::socket>>>(ioc, ws_ctx_);

            auto& lowest = beast::get_lowest_layer(*ws_);
            net::connect(lowest, results);

            if (!SSL_set_tlsext_host_name(
                    ws_->next_layer().native_handle(), ws_host_.c_str()))
            {
                return run_result::handshake_error;
            }

            // Apply the cached session, if any. SSL_set_session up_refs.
            if (SSL_SESSION* sess = cached_session_.load(std::memory_order_acquire))
                SSL_set_session(ws_->next_layer().native_handle(), sess);

            ws_->next_layer().handshake(ssl::stream_base::client);
            ws_->set_option(websocket::stream_base::decorator(
                [](websocket::request_type& req) {
                    req.set(boost::beast::http::field::user_agent, "TrueTest/1.0");
                }));

            std::string target = "/ws/" + current_listen_key();
            ws_->handshake(ws_host_ + ":" + ws_port_, target);

            reached_open = true;
            set_state(lifecycle::open, "user-data stream open");

            beast::flat_buffer buf;
            while (!stop_flag_.load())
            {
                buf.consume(buf.size());
                ws_->read(buf);

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
        constexpr int k_max_attempts = 10;
        constexpr long long k_reset_threshold_ms = 5 * 60 * 1000;
        auto initial = std::chrono::seconds(1);
        auto max_delay = std::chrono::seconds(30);

        int attempt = 0;
        long long last_open_ms = 0;
        auto delay = initial;

        while (!stop_flag_.load())
        {
            if (attempt > 0)
            {
                set_state(lifecycle::connecting,
                          "reconnecting user-data stream (attempt "
                          + std::to_string(attempt + 1) + "/"
                          + std::to_string(k_max_attempts) + ")");
                std::unique_lock<std::mutex> lk(cv_mu_);
                if (cv_.wait_for(lk, delay,
                                 [this] { return stop_flag_.load(); }))
                    break;
                delay = std::min(delay * 2, max_delay);

                // Rotate the listenKey if the reconnect sleep may have
                // outlived its expiration window.
                auto now_ms = static_cast<long long>(binance::server_time_ms());
                if (last_open_ms > 0 &&
                    (now_ms - last_open_ms) >
                    std::chrono::duration_cast<std::chrono::milliseconds>(
                        keepalive_policy_.interval).count())
                {
                    refresh_listen_key_best_effort();
                }
            }

            auto open_start_ms =
                static_cast<long long>(binance::server_time_ms());
            auto r = run_once();
            auto open_end_ms =
                static_cast<long long>(binance::server_time_ms());

            if (r == run_result::stopped) return;

            if (r == run_result::network_error &&
                (open_end_ms - open_start_ms) > k_reset_threshold_ms)
            {
                // Long-running stream hiccuped — reset backoff.
                attempt = 0;
                delay = initial;
                last_open_ms = open_end_ms;
                continue;
            }

            last_open_ms = open_end_ms;

            reconnect_state rs{attempt, open_end_ms, false};
            auto step = decide_next(rs, r, k_max_attempts,
                                    open_end_ms, k_reset_threshold_ms);
            if (step == next_step::stop) return;
            if (step == next_step::give_up)
            {
                set_state(lifecycle::error,
                          "user-data stream: giving up after "
                          + std::to_string(k_max_attempts)
                          + " reconnect attempts");
                return;
            }
            ++attempt;
        }
    }

    void refresh_listen_key_best_effort()
    {
        if (!rest_) return;
        std::string current = current_listen_key();
        if (current.empty()) return;
        try
        {
            auto r = rest_->put_unsigned(
                listen_key_path_, "listenKey=" + current);
            if (r.status < 200 || r.status >= 300)
            {
                auto post = rest_->post_unsigned(listen_key_path_);
                if (post.status >= 200 && post.status < 300)
                {
                    auto key = binance::extract_string(post.body, "listenKey");
                    if (!key.empty())
                    {
                        std::lock_guard<std::mutex> lk(listen_key_mu_);
                        listen_key_ = std::move(key);
                    }
                }
            }
        }
        catch (...) {}
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
                    listen_key_path_, "listenKey=" + key);
                return ka_response{r.status};
            };
            auto post_call = [this](std::string& out_key) {
                auto r = rest_->post_unsigned(listen_key_path_);
                if (r.status >= 200 && r.status < 300)
                {
                    out_key = binance::extract_string(r.body, "listenKey");
                }
                return ka_response{r.status};
            };

            auto wait_fn = [this](std::chrono::seconds delay,
                                  std::atomic<bool>& stop) -> bool {
                std::unique_lock<std::mutex> lk(cv_mu_);
                return cv_.wait_for(
                    lk, delay, [&stop] { return stop.load(); });
            };

            auto out = binance_keepalive_detail::keepalive_tick(
                keepalive_policy_, current, put_call, post_call,
                stop_flag_, wait_fn);

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
                stop_flag_.store(true);
                cv_.notify_all();
                break;
            }
        }
    }

    std::shared_ptr<BinanceRestClient> rest_;
    std::string ws_host_;
    std::string ws_port_;
    std::string listen_key_path_;
    mutable std::mutex listen_key_mu_;
    std::string listen_key_;
    binance_keepalive_policy keepalive_policy_;
    boost::asio::ssl::context ws_ctx_;
    std::atomic<SSL_SESSION*> cached_session_{nullptr};

    std::unique_ptr<boost::beast::websocket::stream<
        boost::beast::ssl_stream<boost::asio::ip::tcp::socket>>> ws_;

    message_cb message_cb_;
    status_cb  status_cb_;

    std::thread reader_;
    std::thread keepalive_;
    std::atomic<bool> stop_flag_{false};

    std::mutex cv_mu_;
    std::condition_variable cv_;

    mutable std::mutex state_mu_;
    lifecycle state_ = lifecycle::closed;
};

#endif // HAS_BINANCE
