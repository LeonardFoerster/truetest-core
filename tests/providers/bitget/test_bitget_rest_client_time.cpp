#include <gtest/gtest.h>

#ifdef HAS_BITGET

#include "providers/bitget/bitget_rest_client.h"
#include "providers/bitget/bitget_time_sync.h"

#include <climits>
#include <cstdlib>
#include <functional>
#include <memory>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <string>
#include <vector>

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace {

using response = BitgetRestClient::response;
using get_fn_t = std::function<response(const std::string&, const std::string&)>;

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
    X509_gmtime_adj(X509_getm_notAfter(x509), 60 * 60 * 24);
    X509_set_pubkey(x509, pkey);
    X509_NAME* name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>("127.0.0.1"),
                               -1, -1, 0);
    X509_set_issuer_name(x509, name);
    char subject_alt_name[] = "IP:127.0.0.1";
    X509_EXTENSION* san = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_subject_alt_name, subject_alt_name);
    if (!san) throw std::runtime_error("failed to create certificate SAN");
    X509_add_ext(x509, san, -1);
    X509_EXTENSION_free(san);
    X509_sign(x509, pkey, EVP_sha256());

    const auto to_string = [](BIO* bio) {
        char* data = nullptr;
        const long len = BIO_get_mem_data(bio, &data);
        return std::string(data, static_cast<std::size_t>(len));
    };
    BIO* cert_bio = BIO_new(BIO_s_mem());
    BIO* key_bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(cert_bio, x509);
    PEM_write_bio_PrivateKey(key_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    auto cert = to_string(cert_bio);
    auto key = to_string(key_bio);
    BIO_free(cert_bio);
    BIO_free(key_bio);
    X509_free(x509);
    EVP_PKEY_free(pkey);
    return {std::move(cert), std::move(key)};
}

class TlsSafetyServer
{
public:
    TlsSafetyServer(const std::string& cert, const std::string& key,
                    int stall_ms, std::string response_body = "{\"code\":\"00000\"}",
                    bool keep_alive = true)
        : acceptor_(ioc_, tcp::endpoint(tcp::v4(), 0))
        , ctx_(ssl::context::tlsv12_server)
        , stall_ms_(stall_ms)
        , response_body_(std::move(response_body))
        , keep_alive_(keep_alive)
    {
        ctx_.use_certificate_chain(net::buffer(cert.data(), cert.size()));
        ctx_.use_private_key(net::buffer(key.data(), key.size()), ssl::context::pem);
    }

    unsigned short port() const { return acceptor_.local_endpoint().port(); }
    int request_count() const { return request_count_.load(std::memory_order_acquire); }
    void start() { thread_ = std::thread([this] { run(); }); }
    ~TlsSafetyServer()
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
            tcp::socket socket(ioc_);
            beast::error_code ec;
            acceptor_.accept(socket, ec);
            if (ec) return;
            ssl::stream<tcp::socket> stream(std::move(socket), ctx_);
            stream.handshake(ssl::stream_base::server, ec);
            if (ec) return;
            beast::flat_buffer buffer;
            http::request<http::string_body> request;
            http::read(stream, buffer, request, ec);
            if (ec) return;
            request_count_.fetch_add(1, std::memory_order_release);
            if (stall_ms_ > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms_));
            http::response<http::string_body> response{http::status::ok, request.version()};
            response.set(http::field::content_type, "application/json");
            response.keep_alive(keep_alive_);
            response.body() = response_body_;
            response.prepare_payload();
            http::write(stream, response, ec);
        }
        catch (...) {}
    }

    net::io_context ioc_;
    tcp::acceptor acceptor_;
    ssl::context ctx_;
    int stall_ms_;
    std::string response_body_;
    bool keep_alive_;
    std::atomic<int> request_count_{0};
    std::thread thread_;
};

// Bitget /api/v2/public/time envelope (serverTime as string, as live).
get_fn_t make_time_ok(long long server_ms, int status = 200,
                      const char* code = "00000")
{
    return [server_ms, status, code](const std::string&, const std::string&) {
        response r;
        r.status = status;
        r.body = std::string("{\"code\":\"") + code
                 + "\",\"msg\":\"success\",\"requestTime\":"
                 + std::to_string(server_ms)
                 + ",\"data\":{\"serverTime\":\""
                 + std::to_string(server_ms) + "\"}}";
        r.business_ok = bitget::is_business_success(status, r.body);
        return r;
    };
}

} // namespace

// --- query sort (prehash) ----------------------------------------------------

TEST(BitgetQuerySort, SortsKeysAlphabetically)
{
    EXPECT_EQ(bitget::sort_query_string("symbol=BTCUSDT&category=USDT-FUTURES"),
              "category=USDT-FUTURES&symbol=BTCUSDT");
    EXPECT_EQ(bitget::sort_query_string("category=USDT-FUTURES&symbol=BTCUSDT"),
              "category=USDT-FUTURES&symbol=BTCUSDT");
    EXPECT_EQ(bitget::sort_query_string("z=1&a=2&m=3"), "a=2&m=3&z=1");
    EXPECT_EQ(bitget::sort_query_string(""), "");
    EXPECT_EQ(bitget::sort_query_string("only=one"), "only=one");
}

// --- business code detection -------------------------------------------------

TEST(BitgetBusinessCode, SuccessCode00000)
{
    EXPECT_TRUE(bitget::is_business_success(
        200, R"({"code":"00000","msg":"success","data":{}})"));
}

TEST(BitgetBusinessCode, Http200BusinessFail)
{
    // Critical Bitget quirk: HTTP 200 + code != "00000" is an error.
    const char* body =
        R"({"code":"40009","msg":"sign signature error","requestTime":1,"data":null})";
    EXPECT_FALSE(bitget::is_business_success(200, body));
    EXPECT_EQ(bitget::extract_business_code(body), "40009");
}

TEST(BitgetBusinessCode, Http4xxIsFail)
{
    EXPECT_FALSE(bitget::is_business_success(
        403, R"({"code":"00000","msg":"success"})"));
}

TEST(BitgetBusinessCode, MissingCodeFailClosed)
{
    EXPECT_FALSE(bitget::is_business_success(200, R"({"msg":"nope"})"));
}

TEST(BitgetBusinessCode, DuplicateOrMalformedCodeFailsClosed)
{
    EXPECT_FALSE(bitget::is_business_success(
        200, R"({"code":"00000","code":"50000"})"));
    EXPECT_TRUE(bitget::extract_business_code(
        R"({"code":"00000","code":"50000"})").empty());
    EXPECT_FALSE(bitget::is_business_success(
        200, R"(garbage "code":"00000")"));
    EXPECT_TRUE(bitget::extract_business_code(
        R"(garbage "code":"00000")").empty());
}

// --- server time parse -------------------------------------------------------

TEST(BitgetServerTimeParse, StringServerTimeInData)
{
    long long ms = 0;
    const char* body =
        R"({"code":"00000","msg":"success","requestTime":1688008631614,)"
        R"("data":{"serverTime":"1688008631614"}})";
    ASSERT_TRUE(bitget::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1688008631614LL);
}

TEST(BitgetServerTimeParse, NumericServerTime)
{
    long long ms = 0;
    const char* body =
        R"({"code":"00000","data":{"serverTime":1710000000000}})";
    ASSERT_TRUE(bitget::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 1710000000000LL);
}

TEST(BitgetServerTimeParse, FallbackRequestTime)
{
    long long ms = 0;
    // No serverTime key — fall back to requestTime.
    const char* body = R"({"code":"00000","requestTime":99,"data":{}})";
    ASSERT_TRUE(bitget::parse_server_time_ms(body, ms));
    EXPECT_EQ(ms, 99LL);
}

TEST(BitgetServerTimeParse, MalformedFails)
{
    long long ms = 0;
    EXPECT_FALSE(bitget::parse_server_time_ms(R"({"code":"00000"})", ms));
    EXPECT_FALSE(bitget::parse_server_time_ms(
        R"({"serverTime":"not-a-number"})", ms));
}

// --- server_time_offset_ms via injectable get_fn -----------------------------

TEST(BitgetClockSkew, OffsetFailsOnNetworkError)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{0, "", false};
    };
    EXPECT_EQ(BitgetRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BitgetClockSkew, OffsetFailsOnHttp4xx)
{
    auto fn = make_time_ok(0, 403);
    EXPECT_EQ(BitgetRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BitgetClockSkew, OffsetFailsOnMalformedBody)
{
    auto fn = [](const std::string&, const std::string&) {
        return response{200, R"({"code":"00000","unrelated":42})", true};
    };
    EXPECT_EQ(BitgetRestClient::server_time_offset_ms(fn), LLONG_MIN);
}

TEST(BitgetClockSkew, OffsetNullCallableReturnsSentinel)
{
    get_fn_t empty;
    EXPECT_EQ(BitgetRestClient::server_time_offset_ms(empty), LLONG_MIN);
}

TEST(BitgetClockSkew, OffsetNearZeroForCurrentServerTime)
{
    // Inject server time ≈ local so offset is small (not LLONG_MIN).
    const long long now = static_cast<long long>(bitget::local_time_ms());
    auto fn = make_time_ok(now);
    auto off = BitgetRestClient::server_time_offset_ms(fn);
    ASSERT_NE(off, LLONG_MIN);
    EXPECT_LT(std::llabs(off), 50); // allow a few ms of test scheduling
}

// --- verify_clock_skew pure logic --------------------------------------------

TEST(BitgetClockSkew, VerifyOkWithinTolerance)
{
    auto r = bitget::verify_clock_skew_offset(500, 2000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, 500);
}

TEST(BitgetClockSkew, VerifyOkAtBoundary)
{
    auto r = bitget::verify_clock_skew_offset(-2000, 2000);
    EXPECT_TRUE(r.ok);
    EXPECT_EQ(r.offset_ms, -2000);
}

TEST(BitgetClockSkew, VerifyFailsPositiveDrift)
{
    auto r = bitget::verify_clock_skew_offset(10'000, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(BitgetClockSkew, VerifyFailsNegativeDrift)
{
    auto r = bitget::verify_clock_skew_offset(-10'000, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("drift"), std::string::npos);
}

TEST(BitgetClockSkew, VerifyReportsFetchFailure)
{
    auto r = bitget::verify_clock_skew_offset(LLONG_MIN, 2000);
    EXPECT_FALSE(r.ok);
    EXPECT_NE(r.note.find("fetch"), std::string::npos);
}

// --- lazy-resync decision ----------------------------------------------------

TEST(BitgetClockResyncDue, NeverSyncedReturnsTrue)
{
    EXPECT_TRUE(BitgetRestClient::resync_due(0, 0, 300'000));
    EXPECT_TRUE(BitgetRestClient::resync_due(1'000'000, 0, 300'000));
    EXPECT_TRUE(BitgetRestClient::resync_due(1'000'000, -1, 300'000));
}

TEST(BitgetClockResyncDue, WithinIntervalReturnsFalse)
{
    EXPECT_FALSE(BitgetRestClient::resync_due(360'000, 300'000, 300'000));
}

TEST(BitgetClockResyncDue, AtIntervalBoundaryIsDue)
{
    EXPECT_TRUE(BitgetRestClient::resync_due(600'000, 300'000, 300'000));
}

TEST(BitgetClockResyncDue, NonPositiveIntervalDisablesLazySync)
{
    EXPECT_FALSE(BitgetRestClient::resync_due(10'000'000, 1, 0));
    EXPECT_FALSE(BitgetRestClient::resync_due(10'000'000, 1, -5));
}

TEST(BitgetRestClientSafetyLane, TimeoutAfterWriteIsSingleAttempt)
{
    auto [cert, key] = make_self_signed_cert();
    TlsSafetyServer server(cert, key, /*stall_ms=*/1000);
    server.start();

    BitgetRestClient client("key", "secret", "pass", "127.0.0.1",
                            std::to_string(server.port()));
    client.add_trusted_ca_for_testing(cert);

    const auto start = std::chrono::steady_clock::now();
    const auto response = client.safety_post_json(
        "/api/v3/trade/place-order", "{}", std::chrono::milliseconds{100});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();

    EXPECT_EQ(response.status, 0);
    EXPECT_FALSE(response.business_ok);
    EXPECT_TRUE(response.request_written);
    EXPECT_LT(elapsed, 800);
    EXPECT_EQ(server.request_count(), 1);
}

TEST(BitgetRestClientSafetyLane, PartialWriteErrorIsAmbiguous)
{
    const beast::error_code aborted = net::error::operation_aborted;
    EXPECT_FALSE(BitgetRestClient::request_may_have_been_written(aborted, 0));
    EXPECT_TRUE(BitgetRestClient::request_may_have_been_written(aborted, 1));
    EXPECT_TRUE(BitgetRestClient::request_may_have_been_written({}, 0));
}

TEST(BitgetRestClientSafetyLane, HttpSuccessBusinessFailureRemainsFailure)
{
    auto [cert, key] = make_self_signed_cert();
    TlsSafetyServer server(cert, key, /*stall_ms=*/0,
                           R"({"code":"40001","msg":"rejected"})");
    server.start();

    BitgetRestClient client("key", "secret", "pass", "127.0.0.1",
                            std::to_string(server.port()));
    client.add_trusted_ca_for_testing(cert);
    const auto response = client.safety_post_json(
        "/api/v3/trade/place-order", "{}", std::chrono::seconds{1});

    EXPECT_EQ(response.status, 200);
    EXPECT_FALSE(response.business_ok);
    EXPECT_TRUE(response.request_written);
    EXPECT_EQ(server.request_count(), 1);
}

TEST(BitgetRestClientSafetyLane, ConnectionCloseSuccessCleansUpSafetyIoState)
{
    auto [cert, key] = make_self_signed_cert();
    TlsSafetyServer server(
        cert, key, /*stall_ms=*/0,
        R"({"code":"00000","msg":"success","data":null})",
        /*keep_alive=*/false);
    server.start();

    BitgetRestClient client("key", "secret", "pass", "127.0.0.1",
                            std::to_string(server.port()));
    client.add_trusted_ca_for_testing(cert);
    const auto response = client.safety_post_json(
        "/api/v3/trade/place-order", "{}", std::chrono::seconds{1});

    EXPECT_EQ(response.status, 200);
    EXPECT_TRUE(response.business_ok);
    EXPECT_TRUE(response.request_written);
    EXPECT_EQ(server.request_count(), 1);
}

TEST(BitgetRestClientColdConnect, SafetyAndNormalTlsHandshakeAreBounded)
{
    net::io_context server_ioc;
    tcp::acceptor acceptor(server_ioc,
        tcp::endpoint(net::ip::address_v4::loopback(), 0));
    auto socket = std::make_shared<tcp::socket>(server_ioc);
    acceptor.async_accept(*socket, [](beast::error_code) {});
    std::thread server([&] { server_ioc.run(); });

    BitgetRestClient client(
        "key", "secret", "pass", "127.0.0.1",
        std::to_string(acceptor.local_endpoint().port()), "/api/v2/public/time");
    const auto safety_started = std::chrono::steady_clock::now();
    const auto safety = client.safety_post_json(
        "/api/v3/trade/place-order", "{}", std::chrono::milliseconds(30));
    EXPECT_EQ(safety.status, 0);
    EXPECT_FALSE(safety.request_written);
    EXPECT_LT(std::chrono::steady_clock::now() - safety_started,
              std::chrono::seconds(1));

    client.set_per_call_timeout(std::chrono::milliseconds(30));
    const auto normal_started = std::chrono::steady_clock::now();
    const auto normal = client.get_unsigned("/api/v2/public/time");
    EXPECT_EQ(normal.status, 0);
    EXPECT_LT(std::chrono::steady_clock::now() - normal_started,
              std::chrono::seconds(1));

    beast::error_code ignored;
    socket->close(ignored);
    acceptor.close(ignored);
    server_ioc.stop();
    server.join();
}

// --- instruments probe (canned JSON) ----------------------------------------

namespace {

// Minimal UTA instruments envelope matching live field names.
constexpr const char* kInstrumentsBtcOk =
    R"({"code":"00000","msg":"success","requestTime":1,"data":[)"
    R"({"symbol":"BTCUSDT","category":"USDT-FUTURES","status":"online",)"
    R"("priceMultiplier":"0.1","quantityMultiplier":"0.001",)"
    R"("minOrderQty":"0.001","minOrderAmount":"5",)"
    R"("makerFeeRate":"0.0002","takerFeeRate":"0.0006"})"
    R"(]})";

constexpr const char* kInstrumentsOffline =
    R"({"code":"00000","msg":"success","data":[)"
    R"({"symbol":"BTCUSDT","status":"offline",)"
    R"("priceMultiplier":"0.1","quantityMultiplier":"0.001"})"
    R"(]})";

constexpr const char* kInstrumentsBusinessFail =
    R"({"code":"40001","msg":"invalid","data":null})";

} // namespace

TEST(BitgetInstruments, QueryBuilder)
{
    EXPECT_EQ(bitget::instruments_query("USDT-FUTURES", "BTCUSDT"),
              "category=USDT-FUTURES&symbol=BTCUSDT");
    EXPECT_EQ(bitget::instruments_query("USDT-FUTURES", ""),
              "category=USDT-FUTURES");
}

TEST(BitgetInstruments, ParseOkTrading)
{
    auto p = bitget::parse_instruments_response(kInstrumentsBtcOk, "BTCUSDT");
    EXPECT_TRUE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_TRUE(p.trading);
    EXPECT_EQ(p.spec.symbol, "BTCUSDT");
    EXPECT_DOUBLE_EQ(p.spec.tick_size, 0.1);
    EXPECT_DOUBLE_EQ(p.spec.lot_size, 0.001);
    EXPECT_DOUBLE_EQ(p.spec.min_qty, 0.001);
    EXPECT_DOUBLE_EQ(p.spec.min_notional, 5.0);
    EXPECT_DOUBLE_EQ(p.spec.maker_rate, 0.0002);
    EXPECT_DOUBLE_EQ(p.spec.taker_rate, 0.0006);
}

TEST(BitgetInstruments, ParseSymbolMissing)
{
    auto p = bitget::parse_instruments_response(kInstrumentsBtcOk, "ETHUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.found);
    EXPECT_NE(p.note.find("not found"), std::string::npos);
}

TEST(BitgetInstruments, ParseOfflineNotTrading)
{
    auto p = bitget::parse_instruments_response(kInstrumentsOffline, "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_TRUE(p.found);
    EXPECT_FALSE(p.trading);
    EXPECT_EQ(p.status, "offline");
}

TEST(BitgetInstruments, ParseBusinessError)
{
    auto p = bitget::parse_instruments_response(kInstrumentsBusinessFail, "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.found);
    EXPECT_NE(p.note.find("40001"), std::string::npos);
}

TEST(BitgetInstruments, ParseEmptyData)
{
    auto p = bitget::parse_instruments_response(
        R"({"code":"00000","data":[]})", "BTCUSDT");
    EXPECT_FALSE(p.ok);
    EXPECT_FALSE(p.found);
}

TEST(BitgetInstruments, RefusesNestedDuplicateAndMalformedDecisionFields)
{
    const std::vector<std::string> malformed = {
        R"({"code":"00000","msg":"success","nested":{"data":[{"symbol":"BTCUSDT","status":"online","priceMultiplier":"0.1","quantityMultiplier":"0.001"}]}})",
        R"({"code":"00000","msg":"success","data":[{"nested":{"symbol":"BTCUSDT"},"status":"online","priceMultiplier":"0.1","quantityMultiplier":"0.001"}]})",
        R"({"code":"00000","msg":"success","data":[{"symbol":"BTCUSDT","symbol":"ETHUSDT","status":"online","priceMultiplier":"0.1","quantityMultiplier":"0.001"}]})",
        R"({"code":"00000","msg":"success","data":[{"symbol":"BTCUSDT","status":"online","priceMultiplier":"0.1","priceMultiplier":"0.2","quantityMultiplier":"0.001"}]})",
        R"({"code":"00000","msg":"success","data":[{"symbol":"BTCUSDT","status":"online","priceMultiplier":"junk","quantityMultiplier":"0.001"}]})",
        R"({"code":"00000","msg":"success","data":[{"symbol":"BTCUSDT","status":"online","priceMultiplier":"0.1","quantityMultiplier":"0.001"},{"symbol":"BTCUSDT","status":"online","priceMultiplier":"0.2","quantityMultiplier":"0.002"}]})",
    };
    for (const auto& body : malformed)
    {
        const auto probe = bitget::parse_instruments_response(
            body, "BTCUSDT");
        EXPECT_FALSE(probe.ok) << body;
    }
}

#endif // HAS_BITGET
