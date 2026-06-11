#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_rest_client.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

// Regression test for the keep-alive I/O-timeout fix.
//
// The kill-switch arms an aggressive per-call timeout right before it fires
// cancel/flatten at shutdown. With persistent Keep-Alive the kill-switch
// reuses a *warm* connection, so the timeout must actually bound the I/O on
// that warm connection — otherwise a still-down LAN could wedge cancel/flatten
// past the kill-switch deadline. (SO_RCVTIMEO/SO_SNDTIMEO do not achieve this
// under Asio synchronous I/O, which is why the client uses async run_for.)
//
// This test establishes a warm TLS connection against a local server, then
// arms a 200 ms timeout and issues a second request to a server that stalls
// for 2 s. The request must abort at ~200 ms.

namespace {

// Build a throwaway self-signed cert + key (PEM) for the local TLS server.
// Short-lived; generated in-process so there is no fixture file to expire.
std::pair<std::string, std::string> make_self_signed_cert()
{
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_getm_notBefore(x509), 0);
    X509_gmtime_adj(X509_getm_notAfter(x509), 60 * 60 * 24);  // 1 day
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("127.0.0.1"), -1, -1, 0);
    X509_set_issuer_name(x509, name);  // self-signed
    X509_sign(x509, pkey, EVP_sha256());

    auto bio_to_string = [](BIO* bio) {
        char* data = nullptr;
        long len = BIO_get_mem_data(bio, &data);
        return std::string(data, static_cast<std::size_t>(len));
    };

    BIO* cbio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cbio, x509);
    std::string cert_pem = bio_to_string(cbio);

    BIO* kbio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(kbio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    std::string key_pem = bio_to_string(kbio);

    BIO_free(cbio);
    BIO_free(kbio);
    X509_free(x509);
    EVP_PKEY_free(pkey);

    return {cert_pem, key_pem};
}

// Minimal blocking TLS/HTTP server on 127.0.0.1: answers the first request
// immediately, then stalls `stall_ms` before answering every later request on
// the same connection.
class TlsStallServer
{
public:
    TlsStallServer(const std::string& cert_pem,
                   const std::string& key_pem,
                   int stall_ms)
        : acceptor_(ioc_, tcp::endpoint(tcp::v4(), 0))
        , server_ctx_(ssl::context::tlsv12_server)
        , stall_ms_(stall_ms)
    {
        server_ctx_.use_certificate_chain(
            net::buffer(cert_pem.data(), cert_pem.size()));
        server_ctx_.use_private_key(
            net::buffer(key_pem.data(), key_pem.size()),
            ssl::context::pem);
    }

    unsigned short port() { return acceptor_.local_endpoint().port(); }

    void start() { thread_ = std::thread([this] { run(); }); }

    ~TlsStallServer()
    {
        beast::error_code ec;
        acceptor_.close(ec);
        if (thread_.joinable()) thread_.join();
    }

private:
    void run()
    {
        try
        {
            tcp::socket sock(ioc_);
            beast::error_code ec;
            acceptor_.accept(sock, ec);
            if (ec) return;

            ssl::stream<tcp::socket> stream(std::move(sock), server_ctx_);
            stream.handshake(ssl::stream_base::server, ec);
            if (ec) return;

            beast::flat_buffer buffer;
            int reqno = 0;
            for (;;)
            {
                http::request<http::string_body> req;
                http::read(stream, buffer, req, ec);
                if (ec) break;

                if (++reqno >= 2 && stall_ms_ > 0)
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(stall_ms_));

                http::response<http::string_body> res{
                    http::status::ok, req.version()};
                res.set(http::field::content_type, "application/json");
                res.body() = "{\"pong\":1}";
                res.prepare_payload();
                http::write(stream, res, ec);
                if (ec) break;
            }
        }
        catch (...) {}
    }

    net::io_context ioc_;
    tcp::acceptor   acceptor_;
    ssl::context    server_ctx_;
    int             stall_ms_;
    std::thread     thread_;
};

}  // namespace

TEST(BinanceRestClientKeepAliveTimeout, WarmConnectionAppliesPerCallTimeout)
{
    auto [cert_pem, key_pem] = make_self_signed_cert();

    TlsStallServer server(cert_pem, key_pem, /*stall_ms=*/2000);
    server.start();
    const unsigned short port = server.port();

    BinanceRestClient client(
        "test-key", "test-secret", "127.0.0.1", std::to_string(port),
        "/api/v3/time");
    client.add_trusted_ca_for_testing(cert_pem);

    // 1) Warm up: a full request/response over a fresh TLS handshake leaves a
    //    live keep-alive connection behind (connected_ == true).
    auto warm = client.get_unsigned("/api/v3/ping");
    ASSERT_EQ(warm.status, 200) << "warm-up request failed: " << warm.body;

    // 2) Arm the aggressive per-call timeout exactly as the kill-switch does,
    //    then issue a second request on the WARM connection. The server stalls
    //    2 s; the fix bounds the read so the call must abort at ~200 ms instead
    //    of waiting out the stall.
    client.set_per_call_timeout(std::chrono::milliseconds(200));

    const auto t0 = std::chrono::steady_clock::now();
    auto r = client.get_unsigned("/api/v3/ping");
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();

    EXPECT_LT(elapsed_ms, 1000)
        << "warm-connection request was not bounded by the per-call timeout "
           "(elapsed " << elapsed_ms << " ms; the 2 s server stall was not "
           "interrupted)";
    EXPECT_EQ(r.status, 0)
        << "a timed-out request must fail rather than return a response";
}

#endif  // HAS_BINANCE
