// Unit tests for Heimdall adapter order normalisation logic.
//
// Tests the JSON parsing logic from BinanceOrderAdapter and OKXOrderAdapter
// using extracted pure normaliser structs — no network, no Aeron, no threads.
//
// Captured ExecEvent structs replace the on_exec_event callback.

#include <messages/ExchangeId.h>
#include <messages/ExecStatus.h>
#include <messages/FeeCurrency.h>
#include <messages/OrderSide.h>
#include <messages/OrderType.h>
#include <messages/RejectReason.h>

#include <boost/json.hpp>
#include <cstring>
#include <gtest/gtest.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

namespace json = boost::json;
using namespace bpt::messages;

static constexpr double kScale = 1e8;

// ── Captured ExecEvent ────────────────────────────────────────────────────────

struct CapturedExecEvent {
    uint64_t order_id;
    uint64_t exchange_order_id;
    ExchangeId::Value exchange_id;
    ExecStatus::Value status;
    OrderSide::Value side;
    OrderType::Value order_type;
    int64_t price;
    uint64_t filled_qty;
    uint64_t remaining_qty;
    RejectReason::Value reject_reason;
    int64_t fee;
    FeeCurrency::Value fee_currency;
    uint64_t exchange_ts_ns;
    uint64_t local_ts_ns;
};

// ── Binance execution report normaliser ──────────────────────────────────────
//
// Mirrors the logic in BinanceOrderAdapter::handle_user_data_message().
// Produces a CapturedExecEvent from a Binance executionReport WS message.

static FeeCurrency::Value parse_fee_currency_binance(const std::string& asset) {
    if (asset == "BTC")
        return FeeCurrency::BTC;
    if (asset == "ETH")
        return FeeCurrency::ETH;
    if (asset == "BNB")
        return FeeCurrency::BNB;
    if (asset == "USDT")
        return FeeCurrency::USDT;
    return FeeCurrency::USDT;
}

class BinanceExecNormaliser {
public:
    explicit BinanceExecNormaliser(std::unordered_map<std::string, uint64_t> cloid_map)
        : cloid_map_(std::move(cloid_map)) {}

    void process(std::string_view payload, uint64_t recv_ns) {
        last_event_.reset();
        auto root = json::parse(payload);
        if (!root.is_object())
            return;
        const auto& obj = root.as_object();

        auto eit = obj.find("e");
        if (eit == obj.end())
            return;
        std::string event_type = std::string(eit->value().as_string());
        if (event_type != "executionReport")
            return;

        std::string exec_type = std::string(obj.at("X").as_string());
        std::string cloid = std::string(obj.at("c").as_string());

        auto id_it = cloid_map_.find(cloid);
        if (id_it == cloid_map_.end())
            return;

        CapturedExecEvent ev;
        ev.order_id = id_it->second;
        ev.exchange_order_id = static_cast<uint64_t>(obj.at("i").as_int64());
        ev.exchange_id = ExchangeId::BINANCE;
        ev.local_ts_ns = recv_ns;
        ev.reject_reason = RejectReason::OK;

        std::string side_str = std::string(obj.at("S").as_string());
        ev.side = (side_str == "BUY") ? OrderSide::BUY : OrderSide::SELL;

        std::string type_str = std::string(obj.at("o").as_string());
        if (type_str == "MARKET")
            ev.order_type = OrderType::MARKET;
        else if (type_str == "LIMIT_MAKER")
            ev.order_type = OrderType::POST_ONLY;
        else
            ev.order_type = OrderType::LIMIT;

        ev.price = static_cast<int64_t>(std::stod(std::string(obj.at("p").as_string())) * kScale);
        ev.filled_qty = static_cast<uint64_t>(std::stod(std::string(obj.at("z").as_string())) * kScale);
        uint64_t total_qty = static_cast<uint64_t>(std::stod(std::string(obj.at("q").as_string())) * kScale);
        ev.remaining_qty = total_qty > ev.filled_qty ? total_qty - ev.filled_qty : 0;

        ev.fee = static_cast<int64_t>(std::stod(std::string(obj.at("n").as_string())) * kScale);
        ev.fee_currency = parse_fee_currency_binance(std::string(obj.at("N").as_string()));

        uint64_t trade_ms = static_cast<uint64_t>(obj.at("T").as_int64());
        ev.exchange_ts_ns = trade_ms * 1'000'000ULL;

        if (exec_type == "NEW") {
            ev.status = ExecStatus::ACKED;
        } else if (exec_type == "TRADE") {
            ev.status = (ev.remaining_qty == 0) ? ExecStatus::FILLED : ExecStatus::PARTIAL;
        } else if (exec_type == "CANCELED" || exec_type == "EXPIRED") {
            ev.status = ExecStatus::CANCELLED;
        } else if (exec_type == "REJECTED") {
            ev.status = ExecStatus::REJECTED;
            ev.reject_reason = RejectReason::EXCHANGE_ERROR;
        } else {
            return;  // ignore PENDING_CANCEL etc.
        }

        last_event_ = ev;
    }

    std::optional<CapturedExecEvent> last_event_;

private:
    std::unordered_map<std::string, uint64_t> cloid_map_;
};

// ── Binance tests ─────────────────────────────────────────────────────────────

TEST(BinanceExecNormaliserTest, NewOrderAcked) {
    BinanceExecNormaliser n({{"G42", 42ULL}});
    const char* payload = R"({
        "e":"executionReport","X":"NEW","c":"G42","S":"BUY","o":"LIMIT",
        "p":"30000.00","q":"1.00","z":"0.00","n":"0.00","N":"USDT",
        "T":1700000000000,"i":12345
    })";
    n.process(payload, 999ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    const auto& ev = *n.last_event_;
    EXPECT_EQ(ev.order_id, 42ULL);
    EXPECT_EQ(ev.exchange_order_id, 12345ULL);
    EXPECT_EQ(ev.exchange_id, ExchangeId::BINANCE);
    EXPECT_EQ(ev.status, ExecStatus::ACKED);
    EXPECT_EQ(ev.side, OrderSide::BUY);
    EXPECT_EQ(ev.order_type, OrderType::LIMIT);
    EXPECT_EQ(ev.price, static_cast<int64_t>(30000.0 * kScale));
    EXPECT_EQ(ev.filled_qty, 0ULL);
    EXPECT_EQ(ev.remaining_qty, static_cast<uint64_t>(1.0 * kScale));
    EXPECT_EQ(ev.reject_reason, RejectReason::OK);
    EXPECT_EQ(ev.exchange_ts_ns, 1700000000000ULL * 1'000'000ULL);
    EXPECT_EQ(ev.local_ts_ns, 999ULL);
}

TEST(BinanceExecNormaliserTest, TradeFilled) {
    BinanceExecNormaliser n({{"G10", 10ULL}});
    // q=total, z=cumulative_filled
    const char* payload = R"({
        "e":"executionReport","X":"TRADE","c":"G10","S":"SELL","o":"LIMIT",
        "p":"29999.00","q":"2.00","z":"2.00","n":"0.05","N":"BNB",
        "T":1700000001000,"i":55555
    })";
    n.process(payload, 1ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    const auto& ev = *n.last_event_;
    EXPECT_EQ(ev.status, ExecStatus::FILLED);
    EXPECT_EQ(ev.side, OrderSide::SELL);
    EXPECT_EQ(ev.filled_qty, static_cast<uint64_t>(2.0 * kScale));
    EXPECT_EQ(ev.remaining_qty, 0ULL);
    EXPECT_EQ(ev.fee_currency, FeeCurrency::BNB);
    EXPECT_EQ(ev.fee, static_cast<int64_t>(0.05 * kScale));
}

TEST(BinanceExecNormaliserTest, TradePartialFill) {
    BinanceExecNormaliser n({{"G20", 20ULL}});
    // q=2.0, z=0.5 → remaining=1.5
    const char* payload = R"({
        "e":"executionReport","X":"TRADE","c":"G20","S":"BUY","o":"LIMIT",
        "p":"25000.00","q":"2.00","z":"0.50","n":"0.02","N":"USDT",
        "T":1700000002000,"i":77777
    })";
    n.process(payload, 2ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    const auto& ev = *n.last_event_;
    EXPECT_EQ(ev.status, ExecStatus::PARTIAL);
    EXPECT_EQ(ev.filled_qty, static_cast<uint64_t>(0.5 * kScale));
    EXPECT_EQ(ev.remaining_qty, static_cast<uint64_t>(1.5 * kScale));
}

TEST(BinanceExecNormaliserTest, OrderCanceled) {
    BinanceExecNormaliser n({{"G30", 30ULL}});
    const char* payload = R"({
        "e":"executionReport","X":"CANCELED","c":"G30","S":"BUY","o":"LIMIT",
        "p":"30000.00","q":"1.00","z":"0.00","n":"0.00","N":"USDT",
        "T":1700000003000,"i":88888
    })";
    n.process(payload, 3ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->status, ExecStatus::CANCELLED);
}

TEST(BinanceExecNormaliserTest, OrderRejected) {
    BinanceExecNormaliser n({{"G40", 40ULL}});
    const char* payload = R"({
        "e":"executionReport","X":"REJECTED","c":"G40","S":"BUY","o":"MARKET",
        "p":"0.00","q":"1.00","z":"0.00","n":"0.00","N":"USDT",
        "T":1700000004000,"i":0
    })";
    n.process(payload, 4ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->status, ExecStatus::REJECTED);
    EXPECT_EQ(n.last_event_->reject_reason, RejectReason::EXCHANGE_ERROR);
}

TEST(BinanceExecNormaliserTest, UnknownCloidDropped) {
    BinanceExecNormaliser n({{"G42", 42ULL}});
    const char* payload = R"({
        "e":"executionReport","X":"NEW","c":"UNKNOWN","S":"BUY","o":"LIMIT",
        "p":"30000.00","q":"1.00","z":"0.00","n":"0.00","N":"USDT",
        "T":1700000000000,"i":12345
    })";
    n.process(payload, 999ULL);
    EXPECT_FALSE(n.last_event_.has_value());
}

TEST(BinanceExecNormaliserTest, NonExecutionReportDropped) {
    BinanceExecNormaliser n({{"G42", 42ULL}});
    const char* payload = R"({"e":"outboundAccountPosition","u":12345})";
    n.process(payload, 1ULL);
    EXPECT_FALSE(n.last_event_.has_value());
}

TEST(BinanceExecNormaliserTest, MarketOrderType) {
    BinanceExecNormaliser n({{"G50", 50ULL}});
    const char* payload = R"({
        "e":"executionReport","X":"NEW","c":"G50","S":"BUY","o":"MARKET",
        "p":"0.00","q":"1.00","z":"0.00","n":"0.00","N":"USDT",
        "T":1700000005000,"i":99999
    })";
    n.process(payload, 5ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->order_type, OrderType::MARKET);
}

TEST(BinanceExecNormaliserTest, PostOnlyOrderType) {
    BinanceExecNormaliser n({{"G60", 60ULL}});
    const char* payload = R"({
        "e":"executionReport","X":"NEW","c":"G60","S":"SELL","o":"LIMIT_MAKER",
        "p":"31000.00","q":"0.5","z":"0.00","n":"0.00","N":"USDT",
        "T":1700000006000,"i":11111
    })";
    n.process(payload, 6ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->order_type, OrderType::POST_ONLY);
}

TEST(BinanceExecNormaliserTest, FeeCurrencyMapping) {
    BinanceExecNormaliser n({{"G70", 70ULL}});
    const char* payload = R"({
        "e":"executionReport","X":"TRADE","c":"G70","S":"BUY","o":"LIMIT",
        "p":"2000.00","q":"1.00","z":"1.00","n":"0.001","N":"ETH",
        "T":1700000007000,"i":22222
    })";
    n.process(payload, 7ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->fee_currency, FeeCurrency::ETH);
}

// ── OKX execution report normaliser ──────────────────────────────────────────
//
// Mirrors the logic in OKXOrderAdapter::handle_message() for "orders" channel.

static FeeCurrency::Value parse_fee_ccy_okx(const std::string& ccy) {
    if (ccy == "BTC")
        return FeeCurrency::BTC;
    if (ccy == "ETH")
        return FeeCurrency::ETH;
    if (ccy == "USDT")
        return FeeCurrency::USDT;
    if (ccy == "USD")
        return FeeCurrency::USD;
    return FeeCurrency::USDT;
}

class OKXExecNormaliser {
public:
    explicit OKXExecNormaliser(std::unordered_map<std::string, uint64_t> cloid_map)
        : cloid_map_(std::move(cloid_map)) {}

    void process(std::string_view payload, uint64_t recv_ns) {
        last_event_.reset();
        auto root = json::parse(payload);
        if (!root.is_object())
            return;
        const auto& obj = root.as_object();

        if (obj.find("event") != obj.end())
            return;
        if (obj.find("data") == obj.end())
            return;

        auto arg_it = obj.find("arg");
        if (arg_it == obj.end())
            return;
        const auto& arg = arg_it->value().as_object();
        std::string channel = std::string(arg.at("channel").as_string());
        if (channel != "orders")
            return;

        const auto& data = obj.at("data").as_array();
        if (data.empty())
            return;

        for (const auto& item : data) {
            const auto& d = item.as_object();
            std::string cloid = std::string(d.at("clOrdId").as_string());
            auto id_it = cloid_map_.find(cloid);
            if (id_it == cloid_map_.end())
                continue;

            CapturedExecEvent ev;
            ev.order_id = id_it->second;
            ev.exchange_order_id = static_cast<uint64_t>(std::stoull(std::string(d.at("ordId").as_string())));
            ev.exchange_id = ExchangeId::OKX;
            ev.local_ts_ns = recv_ns;
            ev.reject_reason = RejectReason::OK;

            std::string side_str = std::string(d.at("side").as_string());
            ev.side = (side_str == "buy") ? OrderSide::BUY : OrderSide::SELL;

            std::string ord_type = std::string(d.at("ordType").as_string());
            if (ord_type == "market")
                ev.order_type = OrderType::MARKET;
            else if (ord_type == "post_only")
                ev.order_type = OrderType::POST_ONLY;
            else
                ev.order_type = OrderType::LIMIT;

            ev.price = static_cast<int64_t>(std::stod(std::string(d.at("px").as_string())) * kScale);
            ev.filled_qty = static_cast<uint64_t>(std::stod(std::string(d.at("fillSz").as_string())) * kScale);
            uint64_t total_qty = static_cast<uint64_t>(std::stod(std::string(d.at("sz").as_string())) * kScale);
            ev.remaining_qty = total_qty > ev.filled_qty ? total_qty - ev.filled_qty : 0;

            ev.fee = static_cast<int64_t>(std::stod(std::string(d.at("fee").as_string())) * kScale);
            ev.fee_currency = parse_fee_ccy_okx(std::string(d.at("feeCcy").as_string()));

            if (auto ts_it = d.find("uTime"); ts_it != d.end())
                ev.exchange_ts_ns =
                    static_cast<uint64_t>(std::stoull(std::string(ts_it->value().as_string()))) * 1'000'000ULL;
            else
                ev.exchange_ts_ns = recv_ns;

            std::string state = std::string(d.at("state").as_string());
            if (state == "live")
                ev.status = ExecStatus::ACKED;
            else if (state == "partially_filled")
                ev.status = ExecStatus::PARTIAL;
            else if (state == "filled")
                ev.status = ExecStatus::FILLED;
            else if (state == "canceled")
                ev.status = ExecStatus::CANCELLED;
            else {
                ev.status = ExecStatus::REJECTED;
                ev.reject_reason = RejectReason::EXCHANGE_ERROR;
            }

            last_event_ = ev;
            break;  // process first match
        }
    }

    std::optional<CapturedExecEvent> last_event_;

private:
    std::unordered_map<std::string, uint64_t> cloid_map_;
};

// ── OKX tests ─────────────────────────────────────────────────────────────────

TEST(OKXExecNormaliserTest, LiveOrderAcked) {
    OKXExecNormaliser n({{"G100", 100ULL}});
    const char* payload = R"({
        "arg":{"channel":"orders","instType":"ANY"},
        "data":[{
            "clOrdId":"G100","ordId":"9999999","side":"buy","ordType":"limit",
            "px":"30000.00","sz":"1.0","fillSz":"0.0","fee":"-0.03","feeCcy":"USDT",
            "state":"live","uTime":"1700000000000"
        }]
    })";
    n.process(payload, 777ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    const auto& ev = *n.last_event_;
    EXPECT_EQ(ev.order_id, 100ULL);
    EXPECT_EQ(ev.exchange_order_id, 9999999ULL);
    EXPECT_EQ(ev.exchange_id, ExchangeId::OKX);
    EXPECT_EQ(ev.status, ExecStatus::ACKED);
    EXPECT_EQ(ev.side, OrderSide::BUY);
    EXPECT_EQ(ev.order_type, OrderType::LIMIT);
    EXPECT_EQ(ev.price, static_cast<int64_t>(30000.0 * kScale));
    EXPECT_EQ(ev.filled_qty, 0ULL);
    EXPECT_EQ(ev.remaining_qty, static_cast<uint64_t>(1.0 * kScale));
    EXPECT_EQ(ev.exchange_ts_ns, 1700000000000ULL * 1'000'000ULL);
}

TEST(OKXExecNormaliserTest, FilledOrder) {
    OKXExecNormaliser n({{"G101", 101ULL}});
    const char* payload = R"({
        "arg":{"channel":"orders","instType":"ANY"},
        "data":[{
            "clOrdId":"G101","ordId":"8888888","side":"sell","ordType":"limit",
            "px":"29500.00","sz":"2.0","fillSz":"2.0","fee":"-0.05","feeCcy":"USDT",
            "state":"filled","uTime":"1700000001000"
        }]
    })";
    n.process(payload, 1ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    const auto& ev = *n.last_event_;
    EXPECT_EQ(ev.status, ExecStatus::FILLED);
    EXPECT_EQ(ev.side, OrderSide::SELL);
    EXPECT_EQ(ev.filled_qty, static_cast<uint64_t>(2.0 * kScale));
    EXPECT_EQ(ev.remaining_qty, 0ULL);
}

TEST(OKXExecNormaliserTest, PartiallyFilledOrder) {
    OKXExecNormaliser n({{"G102", 102ULL}});
    const char* payload = R"({
        "arg":{"channel":"orders","instType":"ANY"},
        "data":[{
            "clOrdId":"G102","ordId":"7777777","side":"buy","ordType":"limit",
            "px":"28000.00","sz":"5.0","fillSz":"2.0","fee":"-0.01","feeCcy":"USDT",
            "state":"partially_filled","uTime":"1700000002000"
        }]
    })";
    n.process(payload, 2ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->status, ExecStatus::PARTIAL);
    EXPECT_EQ(n.last_event_->filled_qty, static_cast<uint64_t>(2.0 * kScale));
    EXPECT_EQ(n.last_event_->remaining_qty, static_cast<uint64_t>(3.0 * kScale));
}

TEST(OKXExecNormaliserTest, CanceledOrder) {
    OKXExecNormaliser n({{"G103", 103ULL}});
    const char* payload = R"({
        "arg":{"channel":"orders","instType":"ANY"},
        "data":[{
            "clOrdId":"G103","ordId":"6666666","side":"buy","ordType":"limit",
            "px":"30000.00","sz":"1.0","fillSz":"0.0","fee":"0","feeCcy":"USDT",
            "state":"canceled","uTime":"1700000003000"
        }]
    })";
    n.process(payload, 3ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->status, ExecStatus::CANCELLED);
}

TEST(OKXExecNormaliserTest, PostOnlyOrderType) {
    OKXExecNormaliser n({{"G104", 104ULL}});
    const char* payload = R"({
        "arg":{"channel":"orders","instType":"ANY"},
        "data":[{
            "clOrdId":"G104","ordId":"5555555","side":"sell","ordType":"post_only",
            "px":"31000.00","sz":"0.5","fillSz":"0.0","fee":"0","feeCcy":"USDT",
            "state":"live","uTime":"1700000004000"
        }]
    })";
    n.process(payload, 4ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->order_type, OrderType::POST_ONLY);
    EXPECT_EQ(n.last_event_->side, OrderSide::SELL);
}

TEST(OKXExecNormaliserTest, UnknownCloidDropped) {
    OKXExecNormaliser n({{"G100", 100ULL}});
    const char* payload = R"({
        "arg":{"channel":"orders","instType":"ANY"},
        "data":[{
            "clOrdId":"UNKNOWN","ordId":"9999999","side":"buy","ordType":"limit",
            "px":"30000.00","sz":"1.0","fillSz":"0.0","fee":"0","feeCcy":"USDT",
            "state":"live","uTime":"1700000000000"
        }]
    })";
    n.process(payload, 1ULL);
    EXPECT_FALSE(n.last_event_.has_value());
}

TEST(OKXExecNormaliserTest, LoginEventIgnored) {
    OKXExecNormaliser n({{"G100", 100ULL}});
    const char* payload = R"({"event":"login","code":"0","msg":""})";
    n.process(payload, 1ULL);
    EXPECT_FALSE(n.last_event_.has_value());
}

TEST(OKXExecNormaliserTest, FeeCurrencyBTC) {
    OKXExecNormaliser n({{"G105", 105ULL}});
    const char* payload = R"({
        "arg":{"channel":"orders","instType":"ANY"},
        "data":[{
            "clOrdId":"G105","ordId":"4444444","side":"buy","ordType":"limit",
            "px":"30000.00","sz":"1.0","fillSz":"1.0","fee":"-0.0001","feeCcy":"BTC",
            "state":"filled","uTime":"1700000005000"
        }]
    })";
    n.process(payload, 5ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->fee_currency, FeeCurrency::BTC);
}

TEST(OKXExecNormaliserTest, RejectStateProducesRejected) {
    OKXExecNormaliser n({{"G106", 106ULL}});
    const char* payload = R"({
        "arg":{"channel":"orders","instType":"ANY"},
        "data":[{
            "clOrdId":"G106","ordId":"3333333","side":"buy","ordType":"limit",
            "px":"30000.00","sz":"1.0","fillSz":"0.0","fee":"0","feeCcy":"USDT",
            "state":"rejected","uTime":"1700000006000"
        }]
    })";
    n.process(payload, 6ULL);

    ASSERT_TRUE(n.last_event_.has_value());
    EXPECT_EQ(n.last_event_->status, ExecStatus::REJECTED);
    EXPECT_EQ(n.last_event_->reject_reason, RejectReason::EXCHANGE_ERROR);
}

}  // namespace
