#pragma once
// Exact median over a std::map<price, count>. Equal prices share one node, so the default
// allocator runs only when a price first appears or its last sample leaves (roughly 3-4x fewer
// nodes than live samples on real captures), not once per tick.
//   insert/erase  O(log k) for a new or vanishing price, otherwise a count bump; k = distinct prices
//   median        O(1) amortised via a tracked entry, as in sorted_vector_freq.hpp
// Handle is the sample's map iterator. It stays valid while any sample at that price is live, and
// the ring holds this one, so erase() needs no lookup. The tracked entry (med_, count_before_med_)
// is re-checked against target_rank() on every median() call and walked to the entry now covering
// it, usually zero or one step. Map iterators never shift, so unlike sorted_vector_freq nothing needs re-indexing.
// Bit-exact.
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>

#include "../fixed_price.hpp"

namespace ticker {

class ExactMapWindow {
  public:
    using MapType = std::map<Price, uint32_t>;
    using Handle = MapType::iterator;

    explicit ExactMapWindow(size_t /*capacity_hint*/) noexcept : med_(counts_.end()) {}

    // Not copyable or movable: med_ may be end(), which points into this map object, not a node.
    ExactMapWindow(const ExactMapWindow&) = delete;
    ExactMapWindow& operator=(const ExactMapWindow&) = delete;

    Handle insert(Price price) {
        const Handle it = counts_.try_emplace(price, 0).first;
        ++it->second;
        if (med_ != counts_.end() && price < med_->first) ++count_before_med_;
        ++size_;
        return it;
    }

    Price value_of(Handle h) const noexcept { return h->first; }

    void erase(Handle h) noexcept {
        assert(size_ > 0 && h->second > 0);
        if (med_ != counts_.end() && h->first < med_->first) --count_before_med_;
        --size_;
        if (--h->second > 0) return;

        // The price's last sample: its node goes. If it was the tracked entry, its successor takes
        // over with the same count before it; only off the end does the pointer step back.
        if (h == med_) {
            med_ = std::next(med_);
            if (med_ == counts_.end() && h != counts_.begin()) {
                med_ = std::prev(h);
                count_before_med_ -= med_->second;
            }
        }
        counts_.erase(h);
        if (counts_.empty()) {
            med_ = counts_.end();
            count_before_med_ = 0;
        }
    }

    // Not const: a miss moves the tracked entry.
    Price median() noexcept {
        assert(size_ > 0);
        ensure_median_valid();
        const size_t local_rank = target_rank() - count_before_med_;
        if (size_ % 2 == 1) return med_->first;

        // Even n: the successor is in the same entry's run or the next entry (every count >= 1).
        const Price hi = (local_rank + 1 < med_->second) ? med_->first : std::next(med_)->first;
        return detail::avg_price(med_->first, hi);
    }

    Price low() const noexcept {
        assert(!counts_.empty());
        return counts_.begin()->first;
    }

    Price high() const noexcept {
        assert(!counts_.empty());
        return counts_.rbegin()->first;
    }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    void clear() noexcept {
        counts_.clear();
        size_ = 0;
        med_ = counts_.end();
        count_before_med_ = 0;
    }

    // Diagnostic, not part of the WindowPolicy concept: distinct live prices k.
    size_t distinct() const noexcept { return counts_.size(); }

  private:
    size_t target_rank() const noexcept { return (size_ - 1) / 2; }

    // The loops end inside counts_ because target < size_ == sum of counts.
    void ensure_median_valid() noexcept {
        if (med_ == counts_.end()) {
            med_ = counts_.begin();
            count_before_med_ = 0;
        }
        const size_t target = target_rank();
        while (target < count_before_med_) {
            --med_;
            count_before_med_ -= med_->second;
        }
        while (target >= count_before_med_ + med_->second) {
            count_before_med_ += med_->second;
            ++med_;
            assert(med_ != counts_.end());
        }
    }

    MapType counts_;  // every count >= 1
    size_t size_ = 0;  // live samples (sum of counts)
    Handle med_;  // tracked entry, or end() when none is tracked
    size_t count_before_med_ = 0;  // live samples at prices below med_->first
};

static_assert(sizeof(ExactMapWindow::Handle) <= 16, "Handle expected to be an iterator-sized POD");

}  // namespace ticker
