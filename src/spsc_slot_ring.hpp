#pragma once
// Lock-free single-producer/single-consumer ring buffer, the per-slot-flag
// variant of spsc_ring.hpp.
//
// Same API and use as SpscRing (try_push / try_pop / approx_size /
// capacity), different synchronization: instead of two shared indices, each
// slot carries its own `full` flag. The producer publishes a slot with a
// release store of full=true; the consumer releases it back with full=false.
// Each side keeps its position (head_ / tail_) private, so neither ever has
// to load the other's index: the producer only reads the flag of the slot it
// is about to fill, the consumer only the flag of the slot it is about to
// drain. When the ring is neither full nor empty they touch different cache
// lines. The cost is padding: every slot is a whole number of cache lines
// (see Slot), so the ring is larger than SpscRing's for the same capacity.
//
// No slot is reserved to tell full from empty (the flag does that), so all
// `Capacity` slots are usable -- one more than SpscRing.
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ticker {

template <typename T, size_t Capacity>
class SpscSlotRing {
    static_assert(std::has_single_bit(Capacity), "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "SpscSlotRing is for POD-ish payloads");

  public:
    SpscSlotRing() = default;
    SpscSlotRing(const SpscSlotRing&) = delete;
    SpscSlotRing& operator=(const SpscSlotRing&) = delete;

    // Producer side. Returns false if the next slot is still full (the
    // consumer has not drained it yet); caller decides what to do.
    bool try_push(const T& item) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        Slot& s = slots_[head & kMask];
        if (s.full.load(std::memory_order_acquire)) return false;
        s.value = item;
        s.full.store(true, std::memory_order_release);
        head_.store(head + 1, std::memory_order_relaxed);
        return true;
    }

    // Consumer side. Returns false if the next slot has not been published.
    bool try_pop(T& out) noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        Slot& s = slots_[tail & kMask];
        if (!s.full.load(std::memory_order_acquire)) return false;
        out = s.value;
        s.full.store(false, std::memory_order_release);
        tail_.store(tail + 1, std::memory_order_relaxed);
        return true;
    }

    // Snapshot depth (approximate under concurrency; fine for high-water
    // marks reported at shutdown / on a monitoring cadence). head_/tail_ are
    // written only by their own side and only read across threads here.
    size_t approx_size() const noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        const size_t t = tail_.load(std::memory_order_relaxed);
        return h > t ? h - t : 0;
    }

    static constexpr size_t capacity() noexcept { return Capacity; }

  private:
    static constexpr size_t kMask = Capacity - 1;
    static constexpr size_t kCacheLine = 64;

    // alignas(kCacheLine) rounds sizeof(Slot) up to a multiple of a cache
    // line, so two adjacent slots never share one.
    struct alignas(kCacheLine) Slot {
        std::atomic<bool> full{false};
        T value{};
    };

    // Monotonic counters (masked on use), each on its own line. Only the
    // owning side writes its counter; approx_size() is the only cross-thread
    // reader.
    alignas(kCacheLine) std::atomic<size_t> head_{0};
    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    alignas(kCacheLine) Slot slots_[Capacity];
};

}  // namespace ticker
