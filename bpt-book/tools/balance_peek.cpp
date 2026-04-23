// balance_peek — minimal Aeron subscriber that reads BalanceSnapshot
// messages from stream 6001 and pretty-prints each row.
//
// Validates the bpt-book SBE encoding end-to-end without needing the
// dashboard or strategy to be wired up yet. Useful for eyeballing
// "does bpt-book see my actual HL balances correctly" during dev.

#include <messages/BalanceSnapshot.h>
#include <messages/MessageHeader.h>

#include <Aeron.h>
#include <FragmentAssembler.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

namespace {

std::atomic<bool> running{true};

void on_signal(int) { running = false; }

std::string rstrip_zeros(const char* s, std::size_t max_len) {
    std::size_t n = 0;
    while (n < max_len && s[n] != '\0' && s[n] != ' ')
        ++n;
    return std::string(s, n);
}

const char* exchange_name(uint8_t id) {
    switch (id) {
        case 0: return "ALL";
        case 1: return "BINANCE";
        case 2: return "OKX";
        case 3: return "HYPERLIQUID";
        case 4: return "DERIBIT";
        default: return "UNKNOWN";
    }
}

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    aeron::Context ctx;
    ctx.aeronDir("/dev/shm/aeron-bpt");
    auto aeron = aeron::Aeron::connect(ctx);

    const std::string channel = "aeron:ipc";
    constexpr int kStreamId = 6001;
    const long sub_id = aeron->addSubscription(channel, kStreamId);
    std::shared_ptr<aeron::Subscription> sub;
    while (running && !(sub = aeron->findSubscription(sub_id)))
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!sub) return 0;

    std::printf("balance_peek subscribed to %s:%d — waiting for snapshots\n",
                channel.c_str(), kStreamId);

    auto handler = [](aeron::AtomicBuffer& buf, aeron::util::index_t offset,
                      aeron::util::index_t length, aeron::Header&) {
        using namespace bpt::messages;

        auto* data = reinterpret_cast<char*>(buf.buffer() + offset);
        MessageHeader hdr;
        hdr.wrap(data, 0, 0, length);
        if (hdr.templateId() != BalanceSnapshot::sbeTemplateId())
            return;

        BalanceSnapshot msg;
        msg.wrapForDecode(data, MessageHeader::encodedLength(), hdr.blockLength(),
                          hdr.version(), length);

        std::printf("\n=== BalanceSnapshot corr=%lu ts=%lu ===\n",
                    static_cast<unsigned long>(msg.correlationId()),
                    static_cast<unsigned long>(msg.timestampNs()));

        auto& group = msg.balances();
        while (group.hasNext()) {
            group.next();
            std::printf("  %-11s  %-7s  %-5s  total=%.6f  free=%.6f  hold=%.6f\n",
                        exchange_name(static_cast<uint8_t>(group.exchangeId())),
                        rstrip_zeros(group.subAccount(), 8).c_str(),
                        rstrip_zeros(group.ccy(), 8).c_str(),
                        static_cast<double>(group.totalE8()) / 1e8,
                        static_cast<double>(group.freeE8()) / 1e8,
                        static_cast<double>(group.holdE8()) / 1e8);
        }
        std::fflush(stdout);
    };

    aeron::FragmentAssembler assembler(handler);
    while (running) {
        const int frags = sub->poll(assembler.handler(), 16);
        if (frags == 0) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::printf("balance_peek exiting\n");
    return 0;
}
