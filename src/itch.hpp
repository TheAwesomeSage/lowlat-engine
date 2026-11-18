// Nasdaq TotalView-ITCH 5.0 adapter. All fields on the wire are big-endian.
// Handled message types (the order-book ones):
//   'A' Add Order (no MPID)   36 bytes     'E' Order Executed   31 bytes
//   'X' Order Cancel          23 bytes     'D' Order Delete     19 bytes
//   'U' Order Replace         35 bytes
// Everything else (system events, stock directory, trades...) is reported as Ignored.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "common.hpp"

namespace ll::itch {

inline uint16_t be16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline uint32_t be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline uint64_t be64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return __builtin_bswap64(v);
}
// ITCH timestamps are 6 bytes (nanoseconds since midnight).
inline uint64_t be48(const uint8_t* p) { return (uint64_t(be16(p)) << 32) | be32(p + 2); }

inline void put16(uint8_t* p, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}
inline void put32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void put64(uint8_t* p, uint64_t v) {
  v = __builtin_bswap64(v);
  std::memcpy(p, &v, 8);
}
inline void put48(uint8_t* p, uint64_t v) {
  put16(p, uint16_t(v >> 32));
  put32(p + 2, uint32_t(v));
}

constexpr size_t kAddLen = 36, kExecLen = 31, kCancelLen = 23, kDeleteLen = 19, kReplaceLen = 35;

enum class Result { Ok, Ignored, Malformed };

// Decode one message (without the 2-byte frame length) into an Event.
// Offsets after the common header: type[0] locate[1..2] tracking[3..4] timestamp[5..10].
inline Result parse(const uint8_t* p, size_t len, Event& ev) {
  if (len == 0) return Result::Malformed;
  switch (p[0]) {
    case 'A':  // order ref[11..18] side[19] shares[20..23] stock[24..31] price[32..35]
      if (len < kAddLen) return Result::Malformed;
      ev.type = EvType::Add;
      ev.ts_ns = be48(p + 5);
      ev.id = be64(p + 11);
      ev.side = (p[19] == 'B') ? Side::Buy : Side::Sell;
      ev.qty = be32(p + 20);
      ev.price = be32(p + 32);
      return Result::Ok;
    case 'E':  // order ref[11..18] executed shares[19..22] match number[23..30]
      if (len < kExecLen) return Result::Malformed;
      ev.type = EvType::Execute;
      ev.ts_ns = be48(p + 5);
      ev.id = be64(p + 11);
      ev.qty = be32(p + 19);
      return Result::Ok;
    case 'X':  // order ref[11..18] cancelled shares[19..22]
      if (len < kCancelLen) return Result::Malformed;
      ev.type = EvType::Cancel;
      ev.ts_ns = be48(p + 5);
      ev.id = be64(p + 11);
      ev.qty = be32(p + 19);
      return Result::Ok;
    case 'D':  // order ref[11..18]
      if (len < kDeleteLen) return Result::Malformed;
      ev.type = EvType::Delete;
      ev.ts_ns = be48(p + 5);
      ev.id = be64(p + 11);
      return Result::Ok;
    case 'U':  // original ref[11..18] new ref[19..26] shares[27..30] price[31..34]
      if (len < kReplaceLen) return Result::Malformed;
      ev.type = EvType::Replace;
      ev.ts_ns = be48(p + 5);
      ev.id = be64(p + 11);
      ev.new_id = be64(p + 19);
      ev.qty = be32(p + 27);
      ev.price = be32(p + 31);
      return Result::Ok;
    default:
      return Result::Ignored;
  }
}

// Inverse of parse(): used by the synthetic feed generator and by the tests.
// `out` must have room for 36 bytes. Returns the message length.
inline size_t encode(const Event& ev, uint8_t* out) {
  std::memset(out, 0, 36);
  put16(out + 1, 1);  // stock locate (always 1: single instrument)
  put48(out + 5, ev.ts_ns);
  switch (ev.type) {
    case EvType::Add:
      out[0] = 'A';
      put64(out + 11, ev.id);
      out[19] = (ev.side == Side::Buy) ? 'B' : 'S';
      put32(out + 20, ev.qty);
      std::memcpy(out + 24, "SYNTH   ", 8);
      put32(out + 32, ev.price);
      return kAddLen;
    case EvType::Execute:
      out[0] = 'E';
      put64(out + 11, ev.id);
      put32(out + 19, ev.qty);
      put64(out + 23, 0);  // match number (unused)
      return kExecLen;
    case EvType::Cancel:
      out[0] = 'X';
      put64(out + 11, ev.id);
      put32(out + 19, ev.qty);
      return kCancelLen;
    case EvType::Delete:
      out[0] = 'D';
      put64(out + 11, ev.id);
      return kDeleteLen;
    case EvType::Replace:
      out[0] = 'U';
      put64(out + 11, ev.id);
      put64(out + 19, ev.new_id);
      put32(out + 27, ev.qty);
      put32(out + 31, ev.price);
      return kReplaceLen;
  }
  return 0;
}

}  // namespace ll::itch
