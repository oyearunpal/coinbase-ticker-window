#include "doctest.h"
#include "spsc_ring.hpp"
#include "spsc_slot_ring.hpp"

#include <atomic>
#include <random>
#include <thread>
#include <vector>

using namespace ticker;

// Both ring implementations share one API and one contract, so every case
// below runs against both. `usable(n)` is how many items an n-slot ring holds
// before try_push() fails: SpscRing reserves one slot to tell full from
// empty, SpscSlotRing (a flag per slot) does not.
struct IndexRingTag {
    template <typename T, size_t N>
    using Ring = SpscRing<T, N>;
    static constexpr size_t usable(size_t n) { return n - 1; }
};
struct SlotRingTag {
    template <typename T, size_t N>
    using Ring = SpscSlotRing<T, N>;
    static constexpr size_t usable(size_t n) { return n; }
};

TEST_CASE_TEMPLATE("SpscRing: push/pop single-threaded ordering", Tag, IndexRingTag, SlotRingTag) {
    typename Tag::template Ring<int, 8> ring;
    constexpr int kUsable = static_cast<int>(Tag::usable(8));
    for (int i = 0; i < kUsable; ++i) CHECK(ring.try_push(i));
    CHECK_FALSE(ring.try_push(99));  // full

    for (int i = 0; i < kUsable; ++i) {
        int v = -1;
        CHECK(ring.try_pop(v));
        CHECK(v == i);
    }
    int v;
    CHECK_FALSE(ring.try_pop(v));  // empty
}

TEST_CASE_TEMPLATE("SpscRing: capacity() reports usable slots", Tag, IndexRingTag, SlotRingTag) {
    typename Tag::template Ring<int, 1024> ring;
    CHECK(ring.capacity() == Tag::usable(1024));
}

TEST_CASE_TEMPLATE("SpscRing: 1M items through producer/consumer, no loss or reordering", Tag, IndexRingTag, SlotRingTag) {
    constexpr size_t N = 1'000'000;
    typename Tag::template Ring<uint64_t, 1 << 12> ring;
    std::atomic<bool> done{false};

    std::thread producer([&] {
        for (uint64_t i = 0; i < N; ++i) {
            while (!ring.try_push(i)) { /* spin */ }
        }
        done.store(true, std::memory_order_release);
    });

    std::vector<uint64_t> received;
    received.reserve(N);
    uint64_t v;
    while (received.size() < N) {
        if (ring.try_pop(v)) received.push_back(v);
    }
    producer.join();

    REQUIRE(received.size() == N);
    for (uint64_t i = 0; i < N; ++i) {
        REQUIRE(received[i] == i);  // no loss, no reordering
    }
}

TEST_CASE_TEMPLATE("SpscRing: the feed_done drain handshake never loses or truncates data", Tag, IndexRingTag, SlotRingTag) {
    // Mirrors main.cpp's / bench_main.cpp's actual shutdown shape: the
    // consumer doesn't know the producer's item count up front and instead
    // watches a separate `done` flag, with one extra pop attempt after
    // observing it before actually exiting (see the comment at that call
    // site for the memory-ordering argument this protects against --
    // unprovable in a unit test on x86/TSO, but this at least proves the
    // *logic* drains fully and terminates under the real code shape,
    // repeated many times with a producer that stops at a random point).
    std::mt19937 rng(4242);
    for (int trial = 0; trial < 500; ++trial) {
        typename Tag::template Ring<uint64_t, 64> ring;  // small on purpose: forces real backpressure
        std::atomic<bool> done{false};
        uint64_t n = 100 + (rng() % 5000);

        std::thread producer([&] {
            for (uint64_t i = 0; i < n; ++i) {
                while (!ring.try_push(i)) std::this_thread::yield();
            }
            done.store(true, std::memory_order_release);
        });

        std::vector<uint64_t> received;
        bool seen_done = false;
        uint64_t v;
        for (;;) {
            if (ring.try_pop(v)) {
                received.push_back(v);
            } else if (seen_done) {
                break;
            } else if (done.load(std::memory_order_acquire)) {
                seen_done = true;
            } else {
                std::this_thread::yield();
            }
        }
        producer.join();

        REQUIRE(received.size() == n);
        for (uint64_t i = 0; i < n; ++i) REQUIRE(received[i] == i);
    }
}

TEST_CASE_TEMPLATE("SpscRing: approx_size tracks occupancy", Tag, IndexRingTag, SlotRingTag) {
    typename Tag::template Ring<int, 16> ring;
    CHECK(ring.approx_size() == 0);
    ring.try_push(1);
    ring.try_push(2);
    CHECK(ring.approx_size() == 2);
    int v;
    ring.try_pop(v);
    CHECK(ring.approx_size() == 1);
}
