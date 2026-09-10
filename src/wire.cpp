// SPDX-License-Identifier: MIT
#include "lob/wire.hpp"

namespace lob::wire {
namespace {

void put_header(Writer& w, MsgType t, std::uint32_t seq, std::uint16_t len) {
    w.u8(static_cast<std::uint8_t>(t));
    w.u8(kVersion);
    w.u16(len);
    w.u32(seq);
}

constexpr std::uint16_t kNewOrderLen   = kHeaderSize + 8 + 4 + 1 + 1 + 1 + 1 + 8 + 8 + 8;  // 48
constexpr std::uint16_t kCancelLen     = kHeaderSize + 8 + 4 + 8;                  // 28
constexpr std::uint16_t kModifyLen     = kHeaderSize + 8 + 4 + 8 + 8 + 8;          // 44
constexpr std::uint16_t kExecLen       = kHeaderSize + 8 + 8 + 4 + 1 + 1 + 1 + 1 + 8 + 8 + 8 + 8 + 8;  // 72
constexpr std::uint16_t kMdTradeLen    = kHeaderSize + 8 + 8 + 8 + 1 + 8;          // 41
constexpr std::uint16_t kMdBookLen     = kHeaderSize + 8 + 8 + 8 + 8 + 8 + 8;      // 56
constexpr std::uint16_t kMdHbLen       = kHeaderSize + 8 + 8;                      // 24

bool header_matches(std::span<const std::uint8_t> f, MsgType t, std::uint16_t len) {
    if (f.size() != len) return false;
    if (static_cast<MsgType>(f[0]) != t) return false;
    if (f[1] != kVersion) return false;
    return true;
}

}  // namespace

std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq,
                   const NewOrder& m) {
    Writer w(out, cap);
    put_header(w, MsgType::NewOrder, seq, kNewOrderLen);
    w.u64(m.order_id);
    w.u32(m.client_id);
    w.u8(static_cast<std::uint8_t>(m.side));
    w.u8(static_cast<std::uint8_t>(m.type));
    w.u8(0);
    w.u8(0);
    w.i64(m.price);
    w.u64(m.quantity);
    w.u64(m.ts_client);
    return w.ok() ? w.size() : 0;
}

std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq,
                   const CancelOrder& m) {
    Writer w(out, cap);
    put_header(w, MsgType::CancelOrder, seq, kCancelLen);
    w.u64(m.order_id);
    w.u32(m.client_id);
    w.u64(m.ts_client);
    return w.ok() ? w.size() : 0;
}

std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq,
                   const ModifyOrder& m) {
    Writer w(out, cap);
    put_header(w, MsgType::ModifyOrder, seq, kModifyLen);
    w.u64(m.order_id);
    w.u32(m.client_id);
    w.i64(m.price);
    w.u64(m.quantity);
    w.u64(m.ts_client);
    return w.ok() ? w.size() : 0;
}

std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq,
                   const ExecReport& m) {
    Writer w(out, cap);
    put_header(w, MsgType::ExecReport, seq, kExecLen);
    w.u64(m.order_id);
    w.u64(m.contra_id);
    w.u32(m.client_id);
    w.u8(static_cast<std::uint8_t>(m.event));
    w.u8(static_cast<std::uint8_t>(m.side));
    w.u8(static_cast<std::uint8_t>(m.reason));
    w.u8(m.flags);
    w.i64(m.price);
    w.u64(m.quantity);
    w.u64(m.leaves);
    w.u64(m.ts_client);
    w.u64(m.ts_engine);
    return w.ok() ? w.size() : 0;
}

std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq,
                   const MdTrade& m) {
    Writer w(out, cap);
    put_header(w, MsgType::MdTrade, seq, kMdTradeLen);
    w.u64(m.stream_seq);
    w.i64(m.price);
    w.u64(m.quantity);
    w.u8(static_cast<std::uint8_t>(m.aggressor));
    w.u64(m.ts_engine);
    return w.ok() ? w.size() : 0;
}

std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq,
                   const MdBookChange& m) {
    Writer w(out, cap);
    put_header(w, MsgType::MdBookChange, seq, kMdBookLen);
    w.u64(m.stream_seq);
    w.i64(m.bid_px);
    w.u64(m.bid_qty);
    w.i64(m.ask_px);
    w.u64(m.ask_qty);
    w.u64(m.ts_engine);
    return w.ok() ? w.size() : 0;
}

std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq,
                   const MdHeartbeat& m) {
    Writer w(out, cap);
    put_header(w, MsgType::MdHeartbeat, seq, kMdHbLen);
    w.u64(m.stream_seq);
    w.u64(m.ts_engine);
    return w.ok() ? w.size() : 0;
}

std::optional<Header> peek_header(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize) return std::nullopt;
    Reader r(bytes.data(), bytes.size());
    Header h{};
    h.type = static_cast<MsgType>(r.u8());
    h.version = r.u8();
    h.length = r.u16();
    h.seq = r.u32();
    if (!r.ok() || h.length < kHeaderSize) return std::nullopt;
    if (bytes.size() < h.length) return std::nullopt;  // frame not complete yet
    return h;
}

std::optional<NewOrder> decode_new_order(std::span<const std::uint8_t> f) {
    if (!header_matches(f, MsgType::NewOrder, kNewOrderLen)) return std::nullopt;
    Reader r(f.data() + kHeaderSize, f.size() - kHeaderSize);
    NewOrder m{};
    m.order_id = r.u64();
    m.client_id = r.u32();
    m.side = static_cast<Side>(r.u8());
    m.type = static_cast<OrderType>(r.u8());
    r.u8();
    r.u8();
    m.price = r.i64();
    m.quantity = r.u64();
    m.ts_client = r.u64();
    return r.ok() ? std::optional{m} : std::nullopt;
}

std::optional<CancelOrder> decode_cancel(std::span<const std::uint8_t> f) {
    if (!header_matches(f, MsgType::CancelOrder, kCancelLen)) return std::nullopt;
    Reader r(f.data() + kHeaderSize, f.size() - kHeaderSize);
    CancelOrder m{};
    m.order_id = r.u64();
    m.client_id = r.u32();
    m.ts_client = r.u64();
    return r.ok() ? std::optional{m} : std::nullopt;
}

std::optional<ModifyOrder> decode_modify(std::span<const std::uint8_t> f) {
    if (!header_matches(f, MsgType::ModifyOrder, kModifyLen)) return std::nullopt;
    Reader r(f.data() + kHeaderSize, f.size() - kHeaderSize);
    ModifyOrder m{};
    m.order_id = r.u64();
    m.client_id = r.u32();
    m.price = r.i64();
    m.quantity = r.u64();
    m.ts_client = r.u64();
    return r.ok() ? std::optional{m} : std::nullopt;
}

std::optional<ExecReport> decode_exec_report(std::span<const std::uint8_t> f) {
    if (!header_matches(f, MsgType::ExecReport, kExecLen)) return std::nullopt;
    Reader r(f.data() + kHeaderSize, f.size() - kHeaderSize);
    ExecReport m{};
    m.order_id = r.u64();
    m.contra_id = r.u64();
    m.client_id = r.u32();
    m.event = static_cast<EventType>(r.u8());
    m.side = static_cast<Side>(r.u8());
    m.reason = static_cast<RejectReason>(r.u8());
    m.flags = r.u8();
    m.price = r.i64();
    m.quantity = r.u64();
    m.leaves = r.u64();
    m.ts_client = r.u64();
    m.ts_engine = r.u64();
    return r.ok() ? std::optional{m} : std::nullopt;
}

std::optional<MdTrade> decode_md_trade(std::span<const std::uint8_t> f) {
    if (!header_matches(f, MsgType::MdTrade, kMdTradeLen)) return std::nullopt;
    Reader r(f.data() + kHeaderSize, f.size() - kHeaderSize);
    MdTrade m{};
    m.stream_seq = r.u64();
    m.price = r.i64();
    m.quantity = r.u64();
    m.aggressor = static_cast<Side>(r.u8());
    m.ts_engine = r.u64();
    return r.ok() ? std::optional{m} : std::nullopt;
}

std::optional<MdBookChange> decode_md_book_change(std::span<const std::uint8_t> f) {
    if (!header_matches(f, MsgType::MdBookChange, kMdBookLen)) return std::nullopt;
    Reader r(f.data() + kHeaderSize, f.size() - kHeaderSize);
    MdBookChange m{};
    m.stream_seq = r.u64();
    m.bid_px = r.i64();
    m.bid_qty = r.u64();
    m.ask_px = r.i64();
    m.ask_qty = r.u64();
    m.ts_engine = r.u64();
    return r.ok() ? std::optional{m} : std::nullopt;
}

std::optional<MdHeartbeat> decode_md_heartbeat(std::span<const std::uint8_t> f) {
    if (!header_matches(f, MsgType::MdHeartbeat, kMdHbLen)) return std::nullopt;
    Reader r(f.data() + kHeaderSize, f.size() - kHeaderSize);
    MdHeartbeat m{};
    m.stream_seq = r.u64();
    m.ts_engine = r.u64();
    return r.ok() ? std::optional{m} : std::nullopt;
}

}  // namespace lob::wire
