// Recorder / Replayer.
// File format: optional 8-byte magic "LLREC001", then frames of [2-byte big-endian length][message].
// The framing is the same as Nasdaq's BinaryFILE ITCH format, so a real ITCH capture can be loaded
// too (the magic is optional).
// Replay is deterministic: it walks the frames in file order and the only clock anywhere is the
// exchange timestamp inside each message - never the wall clock.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common.hpp"
#include "itch.hpp"

namespace ll {

inline constexpr char kMagic[9] = "LLREC001";

class Recorder {
 public:
  explicit Recorder(const std::string& path) : f_(std::fopen(path.c_str(), "wb")) {
    if (f_) std::fwrite(kMagic, 1, 8, f_);
  }
  ~Recorder() {
    if (f_) std::fclose(f_);
  }
  Recorder(const Recorder&) = delete;
  Recorder& operator=(const Recorder&) = delete;

  bool ok() const { return f_ != nullptr; }
  void record(const uint8_t* msg, uint16_t len) {
    uint8_t hdr[2];
    itch::put16(hdr, len);
    std::fwrite(hdr, 1, 2, f_);
    std::fwrite(msg, 1, len, f_);
    ++count_;
  }
  uint64_t count() const { return count_; }

 private:
  FILE* f_;
  uint64_t count_ = 0;
};

class Replayer {
 public:
  struct Frame {
    uint32_t off;
    uint16_t len;
  };

  bool load(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data_.resize(size_t(sz));
    size_t got = std::fread(data_.data(), 1, data_.size(), f);
    std::fclose(f);
    if (got != data_.size()) return false;

    size_t pos = (data_.size() >= 8 && std::memcmp(data_.data(), kMagic, 8) == 0) ? 8 : 0;
    frames_.clear();
    while (pos + 2 <= data_.size()) {
      uint16_t len = itch::be16(&data_[pos]);
      pos += 2;
      if (pos + len > data_.size()) break;  // truncated tail: ignore
      frames_.push_back({uint32_t(pos), len});
      pos += len;
    }
    return true;
  }

  size_t size() const { return frames_.size(); }
  const uint8_t* msg(size_t i) const { return &data_[frames_[i].off]; }
  uint16_t len(size_t i) const { return frames_[i].len; }

  // Parse every frame into Events (ignored message types are skipped).
  std::vector<Event> parse_all(size_t* ignored = nullptr, size_t* malformed = nullptr) const {
    std::vector<Event> out;
    out.reserve(frames_.size());
    size_t ign = 0, bad = 0;
    for (size_t i = 0; i < frames_.size(); ++i) {
      Event e;
      switch (itch::parse(msg(i), len(i), e)) {
        case itch::Result::Ok:
          out.push_back(e);
          break;
        case itch::Result::Ignored:
          ++ign;
          break;
        case itch::Result::Malformed:
          ++bad;
          break;
      }
    }
    if (ignored) *ignored = ign;
    if (malformed) *malformed = bad;
    return out;
  }

 private:
  std::vector<uint8_t> data_;
  std::vector<Frame> frames_;
};

}  // namespace ll
