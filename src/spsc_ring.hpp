#pragma once
// Lock-free single-producer/single-consumer ring buffer.
//
// Used to hand data from the feed thread to the writer thread (StatRecord)
// and, in --capture mode, raw frame bytes to the capture sink. One writer
// (push), one reader (pop); no locks, no CAS loops — just two atomic
// indices, each cache-line-padded so the producer updating `head_` never
// bounces the cache line the consumer is spinning on for `tail_` (and vice
// versa). Capacity must be a power of two so index wraparound is a mask.
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ticker {

template <typename T, size_t Capacity>
class SpscRing {
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing is for POD-ish payloads");

  public:
    SpscRing() = default;
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    // Producer side. Returns false (drops nothing itself — caller decides)
    // if the ring is full; caller is expected to count this via a probe.
    bool try_push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kMask;
        if (next == tail_cached_) {
            tail_cached_ = tail_.load(std::memory_order_acquire);
            if (next == tail_cached_) return false;  // full
        }
        buf_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side. Returns false if the ring is empty.
    bool try_pop(T& out) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_cached_) {
            head_cached_ = head_.load(std::memory_order_acquire);
            if (tail == head_cached_) return false;  // empty
        }
        out = buf_[tail];
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    // Snapshot depth (approximate under concurrency; fine for high-water
    // marks reported at shutdown / on a monitoring cadence).
    size_t approx_size() const noexcept {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_relaxed);
        return (h - t) & kMask;
    }

    static constexpr size_t capacity() noexcept { return Capacity - 1; }  // one slot reserved

  private:
    static constexpr size_t kMask = Capacity - 1;
    static constexpr size_t kCacheLine = 64;

    alignas(kCacheLine) std::atomic<size_t> head_{0};
    // producer's private cached view of tail_, avoids re-reading the atomic
    // (and its cache line) on every push once there's known slack
    alignas(kCacheLine) size_t tail_cached_{0};

    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    alignas(kCacheLine) size_t head_cached_{0};

    alignas(kCacheLine) T buf_[Capacity]{};
};

}  // namespace ticker
