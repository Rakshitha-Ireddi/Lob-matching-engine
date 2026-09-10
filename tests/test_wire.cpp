// SPDX-License-Identifier: MIT
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "lob/wire.hpp"

using namespace lob;
using namespace lob::wire;

TEST(Wire, NewOrderRoundTrip) {
    NewOrder in{424242, 7, Side::Sell, OrderType::Ioc, -12345, 900, 111222333};
    std::array<std::uint8_t, 128> buf{};
    std::size_t n = encode(buf.data(), buf.size(), 5, in);
    ASSERT_GT(n, kHeaderSize);

    auto hdr = peek_header({buf.data(), n});
    ASSERT_TRUE(hdr.has_value());
    EXPECT_EQ(hdr->type, MsgType::NewOrder);
    EXPECT_EQ(hdr->seq, 5u);
    EXPECT_EQ(hdr->length, n);

    auto out = decode_new_order({buf.data(), n});
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->order_id, in.order_id);
    EXPECT_EQ(out->client_id, in.client_id);
    EXPECT_EQ(out->side, in.side);
    EXPECT_EQ(out->type, in.type);
    EXPECT_EQ(out->price, in.price);
    EXPECT_EQ(out->quantity, in.quantity);
    EXPECT_EQ(out->ts_client, in.ts_client);
}

TEST(Wire, ExecReportRoundTrip) {
    ExecReport in{1, 2, 3, EventType::Fill, Side::Buy, RejectReason::None, 0x01,
                  99999, 250, 0, 42, 43};
    std::array<std::uint8_t, 128> buf{};
    std::size_t n = encode(buf.data(), buf.size(), 9, in);
    auto out = decode_exec_report({buf.data(), n});
    ASSERT_TRUE(out.has_value());
    EXPECT_EQ(out->order_id, 1u);
    EXPECT_EQ(out->event, EventType::Fill);
    EXPECT_EQ(out->flags, 0x01);
    EXPECT_EQ(out->price, 99999);
    EXPECT_EQ(out->ts_engine, 43u);
}

TEST(Wire, MarketDataRoundTrips) {
    std::array<std::uint8_t, 128> buf{};

    MdTrade t{0, 10100, 33, Side::Sell, 777};
    std::size_t n = encode(buf.data(), buf.size(), 1, t);
    auto tt = decode_md_trade({buf.data(), n});
    ASSERT_TRUE(tt.has_value());
    EXPECT_EQ(tt->price, 10100);
    EXPECT_EQ(tt->aggressor, Side::Sell);

    MdBookChange b{0, 100, 5, 103, 8, 888};
    n = encode(buf.data(), buf.size(), 2, b);
    auto bb = decode_md_book_change({buf.data(), n});
    ASSERT_TRUE(bb.has_value());
    EXPECT_EQ(bb->bid_px, 100);
    EXPECT_EQ(bb->ask_qty, 8u);
}

TEST(Wire, TruncatedFrameRejected) {
    NewOrder in{1, 1, Side::Buy, OrderType::Limit, 100, 10, 0};
    std::array<std::uint8_t, 128> buf{};
    std::size_t n = encode(buf.data(), buf.size(), 0, in);

    // header says length n, but we only expose n-1 bytes
    EXPECT_FALSE(peek_header({buf.data(), n - 1}).has_value());
    // wrong type decoder
    EXPECT_FALSE(decode_cancel({buf.data(), n}).has_value());
}

TEST(Wire, EncodeFailsWhenBufferTooSmall) {
    NewOrder in{1, 1, Side::Buy, OrderType::Limit, 100, 10, 0};
    std::array<std::uint8_t, 8> tiny{};
    EXPECT_EQ(encode(tiny.data(), tiny.size(), 0, in), 0u);
}

TEST(Wire, LittleEndianOnTheByteStream) {
    MdHeartbeat hb{0x0102030405060708ULL, 0};
    std::array<std::uint8_t, 64> buf{};
    std::size_t n = encode(buf.data(), buf.size(), 0xAABBCCDD, hb);
    ASSERT_GT(n, kHeaderSize);
    // header seq field starts at byte 4, little-endian
    EXPECT_EQ(buf[4], 0xDD);
    EXPECT_EQ(buf[5], 0xCC);
    EXPECT_EQ(buf[6], 0xBB);
    EXPECT_EQ(buf[7], 0xAA);
    // body stream_seq least-significant byte first
    EXPECT_EQ(buf[kHeaderSize], 0x08);
}
