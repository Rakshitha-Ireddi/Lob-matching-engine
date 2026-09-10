// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>

#include "lob/events.hpp"
#include "lob/types.hpp"

// Binary wire protocol for order entry (TCP) and market data (UDP).
//
// Fixed-size, little-endian, 8-byte header + body. All integers are encoded
// explicitly so a capture replays identically regardless of host endianness.
// See docs/adr/0008-binary-wire-protocol.md.
namespace lob::wire {

inline constexpr std::uint8_t kVersion = 1;

enum class MsgType : std::uint8_t {
    // client -> engine
    NewOrder    = 1,
    CancelOrder = 2,
    ModifyOrder = 3,
    // engine -> client
    ExecReport  = 10,
    // engine -> market-data multicast
    MdTrade       = 20,
    MdBookChange  = 21,
    MdHeartbeat   = 22,
};

struct Header {
    MsgType       type;
    std::uint8_t  version;
    std::uint16_t length;   // full frame length, header included
    std::uint32_t seq;      // per-stream sequence number
};
inline constexpr std::size_t kHeaderSize = 8;

struct NewOrder {
    OrderId   order_id;
    ClientId  client_id;
    Side      side;
    OrderType type;
    Price     price;      // kNoPrice for a market order
    Quantity  quantity;
    TsNanos   ts_client;  // client send time, echoed back for RTT
};

struct CancelOrder {
    OrderId  order_id;
    ClientId client_id;
    TsNanos  ts_client;
};

struct ModifyOrder {
    OrderId  order_id;
    ClientId client_id;
    Price    price;
    Quantity quantity;
    TsNanos  ts_client;
};

struct ExecReport {
    OrderId      order_id;
    OrderId      contra_id;
    ClientId     client_id;
    EventType    event;
    Side         side;
    RejectReason reason;
    std::uint8_t flags;      // bit0: aggressor
    Price        price;
    Quantity     quantity;
    Quantity     leaves;
    TsNanos      ts_client;  // echoed
    TsNanos      ts_engine;
};

struct MdTrade {
    std::uint64_t stream_seq;
    Price         price;
    Quantity      quantity;
    Side          aggressor;
    TsNanos       ts_engine;
};

struct MdBookChange {
    std::uint64_t stream_seq;
    Price         bid_px;
    Quantity      bid_qty;
    Price         ask_px;
    Quantity      ask_qty;
    TsNanos       ts_engine;
};

struct MdHeartbeat {
    std::uint64_t stream_seq;
    TsNanos       ts_engine;
};

// --- primitive codec ----------------------------------------------------

class Writer {
public:
    Writer(std::uint8_t* buf, std::size_t cap) : buf_(buf), cap_(cap) {}
    [[nodiscard]] std::size_t size() const noexcept { return n_; }
    [[nodiscard]] bool ok() const noexcept { return ok_; }

    void u8(std::uint8_t v) { put(&v, 1); }
    void u16(std::uint16_t v) { le(v); }
    void u32(std::uint32_t v) { le(v); }
    void u64(std::uint64_t v) { le(v); }
    void i64(std::int64_t v) { le(static_cast<std::uint64_t>(v)); }

private:
    template <class T>
    void le(T v) {
        std::uint8_t tmp[sizeof(T)];
        for (std::size_t i = 0; i < sizeof(T); ++i)
            tmp[i] = static_cast<std::uint8_t>(v >> (8 * i));
        put(tmp, sizeof(T));
    }
    void put(const std::uint8_t* p, std::size_t k) {
        if (n_ + k > cap_) {
            ok_ = false;
            return;
        }
        std::memcpy(buf_ + n_, p, k);
        n_ += k;
    }
    std::uint8_t* buf_;
    std::size_t cap_;
    std::size_t n_ = 0;
    bool ok_ = true;
};

class Reader {
public:
    Reader(const std::uint8_t* buf, std::size_t len) : buf_(buf), len_(len) {}
    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept { return len_ - n_; }

    std::uint8_t u8() {
        std::uint8_t v = 0;
        get(&v, 1);
        return v;
    }
    std::uint16_t u16() { return le<std::uint16_t>(); }
    std::uint32_t u32() { return le<std::uint32_t>(); }
    std::uint64_t u64() { return le<std::uint64_t>(); }
    std::int64_t i64() { return static_cast<std::int64_t>(le<std::uint64_t>()); }

private:
    template <class T>
    T le() {
        std::uint8_t tmp[sizeof(T)] = {};
        get(tmp, sizeof(T));
        T v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i)
            v = static_cast<T>(v | (static_cast<T>(tmp[i]) << (8 * i)));
        return v;
    }
    void get(std::uint8_t* p, std::size_t k) {
        if (n_ + k > len_) {
            ok_ = false;
            std::memset(p, 0, k);
            return;
        }
        std::memcpy(p, buf_ + n_, k);
        n_ += k;
    }
    const std::uint8_t* buf_;
    std::size_t len_;
    std::size_t n_ = 0;
    bool ok_ = true;
};

// --- frame encode -----------------------------------------------------

std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq, const NewOrder&);
std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq, const CancelOrder&);
std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq, const ModifyOrder&);
std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq, const ExecReport&);
std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq, const MdTrade&);
std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq, const MdBookChange&);
std::size_t encode(std::uint8_t* out, std::size_t cap, std::uint32_t seq, const MdHeartbeat&);

// --- frame decode ---------------------------------------------------

// Reads the header of the frame at the front of `bytes`. Returns nullopt if
// fewer than kHeaderSize bytes are available or the frame is not yet complete.
std::optional<Header> peek_header(std::span<const std::uint8_t> bytes);

// Typed body decoders. `frame` must be exactly one complete frame.
std::optional<NewOrder>     decode_new_order(std::span<const std::uint8_t> frame);
std::optional<CancelOrder>  decode_cancel(std::span<const std::uint8_t> frame);
std::optional<ModifyOrder>  decode_modify(std::span<const std::uint8_t> frame);
std::optional<ExecReport>   decode_exec_report(std::span<const std::uint8_t> frame);
std::optional<MdTrade>      decode_md_trade(std::span<const std::uint8_t> frame);
std::optional<MdBookChange> decode_md_book_change(std::span<const std::uint8_t> frame);
std::optional<MdHeartbeat>  decode_md_heartbeat(std::span<const std::uint8_t> frame);

}  // namespace lob::wire
