#pragma once
// Sorted contiguous run of Prices, duplicates stored individually: binary search to locate, memmove
// to shift. Used by SortedVectorWindow (all live samples) and HybridHistogramWindow (its two
// out-of-core tails). A memmove over one array can beat a pointer-chasing tree despite worse Big-O;
// where the crossover lands is measured, not assumed.
// erase() takes a price, not a position, because a stored index is invalidated by every shift.
// Equal prices are indistinguishable, so removing the first match is correct.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstring>
#include <vector>

#include "../fixed_price.hpp"

namespace ticker::detail {

class SortedPriceRun {
  public:
    // capacity_hint is a reservation, not a limit; growth past it is counted in grow_events().
    explicit SortedPriceRun(size_t capacity_hint = 0) {
        if (capacity_hint > 0) v_.reserve(capacity_hint);
    }

    void insert(Price price) {
        // upper_bound keeps the memmove short when the same price prints repeatedly.
        const size_t i = std::upper_bound(v_.begin(), v_.end(), price) - v_.begin();
        const size_t old_n = v_.size();
        const size_t old_cap = v_.capacity();
        v_.resize(old_n + 1);  // POD: one store; may reallocate (counted below)
        if (v_.capacity() != old_cap) ++grow_events_;
        if (i < old_n) std::memmove(v_.data() + i + 1, v_.data() + i, (old_n - i) * sizeof(Price));
        v_[i] = price;
    }

    // Removes one element equal to `price`; false if there is none.
    bool erase(Price price) noexcept {
        auto it = std::lower_bound(v_.begin(), v_.end(), price);
        if (it == v_.end() || *it != price) return false;
        const size_t i = it - v_.begin();
        const size_t n = v_.size();
        if (i + 1 < n) std::memmove(v_.data() + i, v_.data() + i + 1, (n - i - 1) * sizeof(Price));
        v_.pop_back();
        return true;
    }

    Price operator[](size_t rank) const noexcept {
        assert(rank < v_.size());
        return v_[rank];
    }

    Price front() const noexcept {
        assert(!v_.empty());
        return v_.front();
    }
    Price back() const noexcept {
        assert(!v_.empty());
        return v_.back();
    }

    size_t size() const noexcept { return v_.size(); }
    bool empty() const noexcept { return v_.empty(); }
    size_t grow_events() const noexcept { return grow_events_; }
    // Reserved bytes, not live bytes.
    size_t bytes() const noexcept { return v_.capacity() * sizeof(Price); }

    void clear() noexcept { v_.clear(); }

  private:
    std::vector<Price> v_;
    size_t grow_events_ = 0;
};

}  // namespace ticker::detail
