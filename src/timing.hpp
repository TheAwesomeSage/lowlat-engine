// Timing harness. Latency is measured in CPU cycles with rdtscp and converted to
// nanoseconds using a TSC frequency measured at start-up (calibration).
#pragma once
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

namespace ll {

// rdtscp waits for all earlier instructions to finish before reading the counter,
// so it is a good "end of measured region" marker.
inline uint64_t t_end() {
  unsigned aux;
  return __rdtscp(&aux);
}

// "Start" marker: read the counter, then lfence so later instructions cannot start
// executing before the read has happened.
inline uint64_t t_begin() {
  unsigned aux;
  uint64_t t = __rdtscp(&aux);
  _mm_lfence();
  return t;
}

inline void pin_to_core(int core) {
#if defined(_WIN32)
  SetThreadAffinityMask(GetCurrentThread(), 1ull << core);
#else
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(core, &set);
  pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
#endif
}

// Ask the OS scheduler not to preempt this (busy-spinning) thread. Without it, Windows can park a
// spinning thread for milliseconds and the stall shows up as a fake latency spike in the tail.
inline void raise_priority() {
#if defined(_WIN32)
  SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
#endif
}

struct Percentiles {
  double p50 = 0, p99 = 0, p999 = 0, max = 0;
};

struct Clock {
  double ghz = 0;          // TSC cycles per nanosecond
  uint64_t overhead = 0;   // cycles taken by an empty t_begin()/t_end() pair

  double ns(uint64_t cycles) const { return double(cycles) / ghz; }

  // 1) Count TSC ticks over 200 ms of wall-clock time -> frequency.
  // 2) Time an empty region 100k times and keep the median -> measurement overhead.
  static Clock calibrate() {
    using sc = std::chrono::steady_clock;
    Clock c;
    auto w0 = sc::now();
    uint64_t c0 = t_end();
    while (sc::now() - w0 < std::chrono::milliseconds(200)) {
    }
    uint64_t c1 = t_end();
    auto w1 = sc::now();
    double ns = double(std::chrono::duration_cast<std::chrono::nanoseconds>(w1 - w0).count());
    c.ghz = double(c1 - c0) / ns;

    std::vector<uint64_t> d(100000);
    for (auto& x : d) {
      uint64_t a = t_begin();
      uint64_t b = t_end();
      x = b - a;
    }
    std::nth_element(d.begin(), d.begin() + d.size() / 2, d.end());
    c.overhead = d[d.size() / 2];
    return c;
  }
};

// Turn raw cycle samples into p50/p99/p99.9/max in nanoseconds (overhead subtracted).
inline Percentiles summarize(std::vector<uint64_t>& cycles, const Clock& clk) {
  Percentiles p;
  if (cycles.empty()) return p;
  std::sort(cycles.begin(), cycles.end());
  auto at = [&](double q) {
    size_t i = size_t(q * double(cycles.size()));
    if (i >= cycles.size()) i = cycles.size() - 1;
    uint64_t v = cycles[i];
    return clk.ns(v > clk.overhead ? v - clk.overhead : 0);
  };
  p.p50 = at(0.50);
  p.p99 = at(0.99);
  p.p999 = at(0.999);
  uint64_t m = cycles.back();
  p.max = clk.ns(m > clk.overhead ? m - clk.overhead : 0);
  return p;
}

inline void print_header() {
  std::printf("%-26s %12s %14s %10s %10s %10s %12s\n", "stage", "samples", "throughput/s", "p50 ns", "p99 ns",
              "p99.9 ns", "max ns");
}

inline void print_row(const char* name, size_t n, double per_sec, const Percentiles& p) {
  std::printf("%-26s %12zu %14.0f %10.1f %10.1f %10.1f %12.1f\n", name, n, per_sec, p.p50, p.p99, p.p999, p.max);
}

}  // namespace ll
