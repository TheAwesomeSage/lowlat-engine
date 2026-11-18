// Venue-agnostic event type. Every venue adapter (here: ITCH 5.0) turns its wire
// format into this struct; the order book and the algos only ever see Event.
#pragma once
#include <cstdint>

namespace ll {

enum class Side : uint8_t { Buy = 0, Sell = 1 };
enum class EvType : uint8_t { Add, Execute, Cancel, Delete, Replace };

struct Event {
  EvType type{};
  Side side{};          // Add only. For other types the book looks the side up from the order id.
  uint32_t qty = 0;     // Add: shares. Execute: shares executed. Cancel: shares cancelled. Replace: new shares.
  uint32_t price = 0;   // 1/10000 dollar (ITCH native). Add and Replace only.
  uint64_t ts_ns = 0;   // exchange timestamp, nanoseconds since midnight
  uint64_t id = 0;      // order reference number (Replace: the ORIGINAL order)
  uint64_t new_id = 0;  // Replace only: the new order reference
};

}  // namespace ll
