// Single-producer single-consumer queues.
//   SpscQueue  : lock-free ring buffer. One thread calls push(), one thread calls pop().
//   MutexQueue : same interface, same ring buffer, but guarded by a std::mutex (the baseline).
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <vector>

namespace ll {

constexpr size_t kCacheLine = 64;

template <class T, size_t N>
class SpscQueue {
  static_assert((N & (N - 1)) == 0, "N must be a power of two (index wrap is a mask, not a modulo)");
  static_assert(std::is_trivially_copyable_v<T>);
  static constexpr size_t kMask = N - 1;

  // Each side's hot variables live on their OWN cache line. If head and tail shared a line, every
  // push would invalidate the consumer's copy of that line and vice versa ("false sharing"), even
  // though the two threads never write the same variable.
  struct alignas(kCacheLine) ProducerSide {
    std::atomic<size_t> tail{0};  // next slot to write (written by producer only)
    size_t cached_head = 0;       // producer's last-seen copy of head, avoids re-reading the atomic
  };
  struct alignas(kCacheLine) ConsumerSide {
    std::atomic<size_t> head{0};  // next slot to read (written by consumer only)
    size_t cached_tail = 0;
  };

 public:
  bool push(const T& v) {
    size_t t = p_.tail.load(std::memory_order_relaxed);
    if (t - p_.cached_head == N) {  // looks full: refresh our view of head
      p_.cached_head = c_.head.load(std::memory_order_acquire);
      if (t - p_.cached_head == N) return false;
    }
    buf_[t & kMask] = v;
    p_.tail.store(t + 1, std::memory_order_release);  // publishes the write above to the consumer
    return true;
  }

  bool pop(T& out) {
    size_t h = c_.head.load(std::memory_order_relaxed);
    if (h == c_.cached_tail) {  // looks empty: refresh our view of tail
      c_.cached_tail = p_.tail.load(std::memory_order_acquire);
      if (h == c_.cached_tail) return false;
    }
    out = buf_[h & kMask];
    c_.head.store(h + 1, std::memory_order_release);  // hands the slot back to the producer
    return true;
  }

 private:
  ProducerSide p_;
  ConsumerSide c_;
  alignas(kCacheLine) T buf_[N];
};

template <class T, size_t N>
class MutexQueue {
 public:
  MutexQueue() : buf_(N) {}

  bool push(const T& v) {
    std::lock_guard<std::mutex> g(m_);
    if (tail_ - head_ == N) return false;
    buf_[tail_ % N] = v;
    ++tail_;
    return true;
  }
  bool pop(T& out) {
    std::lock_guard<std::mutex> g(m_);
    if (head_ == tail_) return false;
    out = buf_[head_ % N];
    ++head_;
    return true;
  }

 private:
  std::mutex m_;
  std::vector<T> buf_;
  size_t head_ = 0, tail_ = 0;
};

}  // namespace ll
