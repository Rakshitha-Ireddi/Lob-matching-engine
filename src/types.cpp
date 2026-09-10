// SPDX-License-Identifier: MIT
#include "lob/types.hpp"

namespace lob {

const char* to_string(Side s) noexcept {
    switch (s) {
        case Side::Buy:  return "BUY";
        case Side::Sell: return "SELL";
    }
    return "?";
}

const char* to_string(OrderType t) noexcept {
    switch (t) {
        case OrderType::Limit:    return "LIMIT";
        case OrderType::Market:   return "MARKET";
        case OrderType::Ioc:      return "IOC";
        case OrderType::PostOnly: return "POST_ONLY";
        case OrderType::Fok:      return "FOK";
    }
    return "?";
}

const char* to_string(RejectReason r) noexcept {
    switch (r) {
        case RejectReason::None:               return "NONE";
        case RejectReason::UnknownOrder:       return "UNKNOWN_ORDER";
        case RejectReason::DuplicateOrderId:   return "DUPLICATE_ORDER_ID";
        case RejectReason::PriceOutOfBand:     return "PRICE_OUT_OF_BAND";
        case RejectReason::ZeroQuantity:       return "ZERO_QUANTITY";
        case RejectReason::PostOnlyWouldCross: return "POST_ONLY_WOULD_CROSS";
        case RejectReason::FokUnfillable:      return "FOK_UNFILLABLE";
        case RejectReason::BookHalted:         return "BOOK_HALTED";
        case RejectReason::PoolExhausted:      return "POOL_EXHAUSTED";
        case RejectReason::SelfTradePrevented: return "SELF_TRADE_PREVENTED";
    }
    return "?";
}

}  // namespace lob
