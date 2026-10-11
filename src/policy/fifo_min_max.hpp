#pragma once
// Exact sliding min/max via monotonic deques, shared by the histogram policies so they only have to
// answer the median. Requires erase in strict FIFO order, which SlidingWindowStats::on_tick
// guarantees. Worst case (a strictly monotonic price trend) a deque grows to O(live samples);
// realistically it stays small.
#include <cassert>
#include <cstdint>

#include "../fixed_price.hpp"
#include "../sliding_window.hpp"

namespace ticker::detail {

class FifoMinMax {
  public:
    FifoMinMax() : min_(64), max_(64) {}

    void push(Price price) noexcept {
        const uint32_t seq = next_insert_seq_++;
        while (!min_.empty() && min_.back().price >= price) min_.pop_back();
        min_.push_back(Entry{price, seq});
        while (!max_.empty() && max_.back().price <= price) max_.pop_back();
        max_.push_back(Entry{price, seq});
    }

    // FIFO order only. A no-op unless the deque front is the entry being evicted; most were already
    // popped by a later push().
    void pop_oldest() noexcept {
        if (!min_.empty() && min_.front().seq == next_evict_seq_) min_.pop_front();
        if (!max_.empty() && max_.front().seq == next_evict_seq_) max_.pop_front();
        ++next_evict_seq_;
    }

    Price low() noexcept {
        assert(!min_.empty());
        return min_.front().price;
    }

    Price high() noexcept {
        assert(!max_.empty());
        return max_.front().price;
    }

    void reset() noexcept {
        min_.reset();
        max_.reset();
        next_insert_seq_ = 0;
        next_evict_seq_ = 0;
    }

  private:
    // seq is only compared for equality and both counters wrap together, so 32 bits is safe while
    // fewer than 2^32 samples are live.
    struct Entry {
        Price price;
        uint32_t seq;
    };
    static_assert(sizeof(Entry) == 8);

    Ring<Entry> min_;
    Ring<Entry> max_;
    uint32_t next_insert_seq_ = 0;
    uint32_t next_evict_seq_ = 0;
};

}  // namespace ticker::detail
