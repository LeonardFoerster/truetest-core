#include <gtest/gtest.h>

#ifdef HAS_BINANCE

#include "providers/binance/binance_rest_client.h"

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

// The kill-switch timeout must bound I/O on an already-open keep-alive
// connection, not just on connect. SO_RCVTIMEO/SO_SNDTIMEO don't do that
// under Asio sync I/O, hence the client's async run_for.
// Setup: warm TLS connection to a local server, 200 ms timeout armed,
// second request against a server that stalls 2 s -> must abort at ~200 ms.

namespace {

// Build a throwaway self-signed cert + key (PEM) for the local TLS server.
// Short-lived; generated in-process so there is no fixture file to expire.
std::pair<std::string, std::string> make_self_signed_cert(
    const char* ip_identity = "127.0.0.1")
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
        reinterpret_cast<const unsigned char*>(ip_identity), -1, -1, 0);
    X509_set_issuer_name(x509, name);  // self-signed
    std::string subject_alt_name = std::string("IP:") + ip_identity;
    X509_EXTENSION* san = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_subject_alt_name, subject_alt_name.data());
    if (!san) throw std::runtime_error("failed to create certificate SAN");
    X509_add_ext(x509, san, -1);
    X509_EXTENSION_free(san);
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

TEST(BinanceRestClientSafetyLane, PartialWriteErrorIsAmbiguous)
{
    const beast::error_code aborted = net::error::operation_aborted;
    EXPECT_FALSE(BinanceRestClient::request_may_have_been_written(aborted, 0));
    EXPECT_TRUE(BinanceRestClient::request_may_have_been_written(aborted, 1));
    EXPECT_TRUE(BinanceRestClient::request_may_have_been_written({}, 0));
}

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

    auto warm = client.get_unsigned("/api/v3/ping");
    ASSERT_EQ(warm.status, 200) << "warm-up request failed: " << warm.body;

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

TEST(BinanceRestClientKeepAliveTimeout, SafetyLaneReusesOneBoundedConnection)
{
    auto [cert_pem, key_pem] = make_self_signed_cert();

    TlsStallServer server(cert_pem, key_pem, /*stall_ms=*/0);
    server.start();

    BinanceRestClient client(
        "test-key", "test-secret", "127.0.0.1",
        std::to_string(server.port()), "/api/v3/time");
    client.add_trusted_ca_for_testing(cert_pem);

    const auto first = client.safety_post(
        "/api/v3/order", "symbol=BTCUSDT", std::chrono::seconds(1));
    const auto second = client.safety_post(
        "/api/v3/order", "symbol=BTCUSDT", std::chrono::seconds(1));

    EXPECT_EQ(first.status, 200);
    EXPECT_EQ(second.status, 200);
}

TEST(TlsPeerIdentity, RejectsTrustedCertificateForWrongIp)
{
    auto [cert_pem, key_pem] = make_self_signed_cert("127.0.0.2");
    TlsStallServer server(cert_pem, key_pem, /*stall_ms=*/0);
    server.start();

    net::io_context ioc;
    ssl::context ctx(ssl::context::tlsv12_client);
    ctx.set_verify_mode(ssl::verify_peer);
    ctx.add_certificate_authority(net::buffer(cert_pem.data(), cert_pem.size()));
    ssl::stream<tcp::socket> stream(ioc, ctx);
    ASSERT_TRUE(provider_ws::configure_tls_peer_identity(
        stream.native_handle(), "127.0.0.1"));

    beast::error_code ec;
    tcp::resolver resolver(ioc);
    const auto endpoints = resolver.resolve(
        "127.0.0.1", std::to_string(server.port()), ec);
    ASSERT_FALSE(ec);
    net::connect(stream.next_layer(), endpoints, ec);
    ASSERT_FALSE(ec);
    stream.handshake(ssl::stream_base::client, ec);
    EXPECT_TRUE(ec)
        << "a trusted certificate for a different IP address must be rejected";
}

TEST(BinanceRestClientColdConnect, SafetyAndNormalTlsHandshakeAreBounded)
{
    net::io_context server_ioc;
    tcp::acceptor acceptor(server_ioc,
        tcp::endpoint(net::ip::address_v4::loopback(), 0));
    auto socket = std::make_shared<tcp::socket>(server_ioc);
    acceptor.async_accept(*socket, [](beast::error_code) {});
    std::thread server([&] { server_ioc.run(); });

    BinanceRestClient client(
        "key", "secret", "127.0.0.1",
        std::to_string(acceptor.local_endpoint().port()), "/api/v3/time");
    const auto safety_started = std::chrono::steady_clock::now();
    const auto safety = client.safety_post(
        "/api/v3/order", "symbol=BTCUSDT", std::chrono::milliseconds(30));
    EXPECT_EQ(safety.status, 0);
    EXPECT_FALSE(safety.request_written);
    EXPECT_LT(std::chrono::steady_clock::now() - safety_started,
              std::chrono::seconds(1));

    client.set_per_call_timeout(std::chrono::milliseconds(30));
    const auto normal_started = std::chrono::steady_clock::now();
    const auto normal = client.get_unsigned("/api/v3/ping");
    EXPECT_EQ(normal.status, 0);
    EXPECT_LT(std::chrono::steady_clock::now() - normal_started,
              std::chrono::seconds(1));

    beast::error_code ignored;
    socket->close(ignored);
    acceptor.close(ignored);
    server_ioc.stop();
    server.join();
}

#endif  // HAS_BINANCE
