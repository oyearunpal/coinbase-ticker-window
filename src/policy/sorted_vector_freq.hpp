#pragma once
// Like sorted_vector.hpp but equal prices collapse into one (price, count) entry, so
// memory and memmove size are the number of DISTINCT prices k. It wins when prices repeat heavily
// (~3-6 samples per distinct price on real captures) and is strictly worse than sorted_vector when every price
// is distinct (8-byte entries against sorted_vector's 4-byte prices).
//   insert/erase  O(log k) locate; memmove only when a price appears or disappears
//   median        O(1) via a lazily validated pointer; on a miss, a walk from the pointer
// The pointer (med_idx_, count_before_med_) is re-checked against target_rank() on every median()
// call. On a miss it walks to the entry that now covers the target rank, which is usually a
// neighbour, so the cost is the walk distance, not k. Adding or removing an entry shifts indices, so
// med_idx_ is corrected for that. Only the first median() of an empty window scans from the front.
// Bit-exact.
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "../fixed_price.hpp"

namespace ticker {

class SortedVectorFreqWindow {
  public:
    // Price only: erase() re-locates by value, since a stored index would be invalidated by shifts.
    struct Handle {
        Price price;
    };

    // 8 bytes: a 4-byte price and a 4-byte count.
    struct Entry {
        Price price;
        uint32_t count;
    };

    // capacity_hint counts samples, but this vector holds one entry per distinct price (2.7x-6x
    // fewer on real captures). Reserve half: more than the measured p90 ratio, so it rarely
    // regrows.
    static constexpr size_t kSamplesPerDistinct = 2;

    explicit SortedVectorFreqWindow(size_t capacity_hint) {
        const size_t reserve_n = capacity_hint / kSamplesPerDistinct;
        if (reserve_n > 0) entries_.reserve(reserve_n);
    }

    Handle insert(Price price) {
        const size_t i = lower_bound_index(price);
        // Existing price: bump the count, no shift.
        if (i < entries_.size() && entries_[i].price == price) {
            if (med_valid_ && i < med_idx_) ++count_before_med_;
            ++entries_[i].count;
        } else {
            // A new entry at i shifts the tracked entry right and counts as one more sample before
            // the median.
            if (med_valid_ && i <= med_idx_) {
                ++count_before_med_;
                ++med_idx_;
            }
            const size_t old_n = entries_.size();
            entries_.resize(old_n + 1);
            if (i < old_n) {
                std::memmove(entries_.data() + i + 1, entries_.data() + i, (old_n - i) * sizeof(Entry));
            }
            entries_[i] = Entry{price, 1};
        }
        ++size_;
        return Handle{price};
    }

    void erase(Handle h) noexcept {
        const size_t i = lower_bound_index(h.price);
        assert(i < entries_.size() && entries_[i].price == h.price &&
               "erase() of a price not in the run -- Handle/window mismatch");
        assert(entries_[i].count > 0);
        if (entries_[i].count == 1) {
            // The entry disappears and later indices shift left.
            if (med_valid_ && i < med_idx_) {
                --count_before_med_;
                --med_idx_;
            }
            const size_t n = entries_.size();
            if (i + 1 < n) {
                std::memmove(entries_.data() + i, entries_.data() + i + 1, (n - i - 1) * sizeof(Entry));
            }
            entries_.pop_back();
            // If it was the tracked entry, med_idx_ now names its successor and count_before_med_
            // is still right, so median() just walks on from there. Only when the tracked entry was
            // the last one does the pointer fall off the end: step back onto the new last entry.
            if (med_valid_ && med_idx_ == entries_.size()) {
                if (entries_.empty()) {
                    med_valid_ = false;
                } else {
                    --med_idx_;
                    count_before_med_ -= entries_[med_idx_].count;
                }
            }
        } else {
            if (med_valid_ && i < med_idx_) --count_before_med_;
            --entries_[i].count;
        }
        assert(size_ > 0);
        --size_;
    }

    Price value_of(Handle h) const noexcept { return h.price; }

    // Not const: a rescan refreshes the tracked pointer.
    Price median() noexcept {
        assert(size_ > 0);
        ensure_median_valid();
        const Entry& e = entries_[med_idx_];

        if (size_ % 2 == 1) return e.price;

        // Even n: the successor is in the same entry's run or the next entry (every count >= 1, so
        // no gaps).
        const size_t local_rank = target_rank() - count_before_med_;
        const Price hi = (local_rank + 1 < e.count) ? e.price : entries_[med_idx_ + 1].price;
        return detail::avg_price(e.price, hi);
    }

    Price low() const noexcept {
        assert(!entries_.empty());
        return entries_.front().price;
    }

    Price high() const noexcept {
        assert(!entries_.empty());
        return entries_.back().price;
    }

    size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    void clear() noexcept {
        entries_.clear();
        size_ = 0;
        med_valid_ = false;
        med_idx_ = 0;
        count_before_med_ = 0;
    }

    // Diagnostics below are not part of the WindowPolicy concept.
    size_t policy_bytes() const noexcept { return entries_.capacity() * sizeof(Entry); }
    // Distinct live prices k; size() / distinct() is the bet this policy makes.
    size_t distinct() const noexcept { return entries_.size(); }
    // Entries stepped over by fallback walks only.
    uint64_t median_scan_steps() const noexcept { return median_scan_steps_; }
    // median() calls that missed the tracked pointer.
    uint64_t fallback_count() const noexcept { return fallback_count_; }

  private:
    size_t lower_bound_index(Price price) const noexcept {
        auto it = std::lower_bound(entries_.begin(), entries_.end(), price,
                                    [](const Entry& e, Price p) { return e.price < p; });
        return static_cast<size_t>(it - entries_.begin());
    }

    size_t target_rank() const noexcept { return (size_ - 1) / 2; }

    // O(1) if the tracked entry still covers target_rank(). Otherwise walk from it toward the target;
    // the loops end inside entries_ because target < size_ == sum of counts. With no valid pointer
    // (first call on an empty-then-filled window) scan from the front.
    void ensure_median_valid() noexcept {
        const size_t target = target_rank();
        if (med_valid_ && count_before_med_ <= target && target < count_before_med_ + entries_[med_idx_].count) {
            return;
        }
        ++fallback_count_;

        if (!med_valid_) {
            med_idx_ = 0;
            count_before_med_ = 0;
            med_valid_ = true;
            ++median_scan_steps_;
        }
        while (target < count_before_med_) {
            --med_idx_;
            count_before_med_ -= entries_[med_idx_].count;
            ++median_scan_steps_;
        }
        while (target >= count_before_med_ + entries_[med_idx_].count) {
            count_before_med_ += entries_[med_idx_].count;
            ++med_idx_;
            ++median_scan_steps_;
            assert(med_idx_ < entries_.size());
        }
    }

    std::vector<Entry> entries_;  // sorted ascending by price, every count >= 1
    size_t size_ = 0;  // live SAMPLES (sum of counts), not entries
    uint64_t median_scan_steps_ = 0;

    bool med_valid_ = false;
    size_t med_idx_ = 0;  // index into entries_, valid only if med_valid_
    size_t count_before_med_ = 0;  // live samples in entries_[0..med_idx_)
    uint64_t fallback_count_ = 0;
};

static_assert(sizeof(SortedVectorFreqWindow::Handle) == 4, "price only -- erase re-locates by value");
static_assert(sizeof(SortedVectorFreqWindow::Entry) == 8, "4-byte price + 4-byte count");

}  // namespace ticker
