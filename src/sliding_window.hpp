#pragma once
// Time-based sliding window around a WindowPolicy. This class owns the
// ring of {timestamp, Handle} samples, eviction, and the running sum for the
// mean; the policy (policy/window_policy.hpp) provides median/low/high.
// The window length is a runtime value.
//
// Window is half-open, (t - window_ns, t], on exchange event time clamped to
// be non-decreasing, so a replayed capture gives identical output.
#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include "fixed_price.hpp"
#include "probe.hpp"
#include "policy/window_policy.hpp"

namespace ticker {

// Initial per-symbol ring capacity (doubles when full) and policy reservation hint.
inline constexpr size_t kCapacityHint = 4096;

// Single-threaded growable ring (not the cross-thread SpscRing). Growth is
// counted so it shows up in the run summary.
template <typename T>
class Ring {
  public:
    // Capacity is rounded up to a power of two so wrapping is `& mask_`, not `%`.
    explicit Ring(size_t capacity) : buf_(std::bit_ceil(capacity)), mask_(buf_.size() - 1) {}

    bool empty() const noexcept { return size_ == 0; }
    size_t size() const noexcept { return size_; }
    size_t capacity() const noexcept { return buf_.size(); }
    size_t grow_events() const noexcept { return grow_events_; }

    T& front() noexcept {
        assert(!empty());
        return buf_[head_];
    }

    void pop_front() noexcept {
        assert(!empty());
        head_ = (head_ + 1) & mask_;
        --size_;
    }

    void push_back(const T& v) {
        if (size_ == buf_.size()) grow();
        buf_[(head_ + size_) & mask_] = v;
        ++size_;
    }

    T& back() noexcept {
        assert(!empty());
        return buf_[(head_ + size_ - 1) & mask_];
    }

    // back()/pop_back() are used by policies' own internal rings.
    void pop_back() noexcept {
        assert(!empty());
        --size_;
    }

    // Index relative to front(); used to replay live samples on a rebuild.
    T& at(size_t i) noexcept {
        assert(i < size_);
        return buf_[(head_ + i) & mask_];
    }

    // Empties the ring but keeps its grown capacity.
    void reset() noexcept { head_ = 0; size_ = 0; }

  private:
    void grow() {
        std::vector<T> nb(buf_.size() * 2);
        // Live elements are at most two contiguous runs: head_..end, then 0..tail.
        const size_t first = std::min(size_, buf_.size() - head_);
        std::copy_n(buf_.begin() + head_, first, nb.begin());
        std::copy_n(buf_.begin(), size_ - first, nb.begin() + first);
        buf_ = std::move(nb);
        mask_ = buf_.size() - 1;
        head_ = 0;
        ++grow_events_;
    }

    std::vector<T> buf_;
    size_t mask_;
    size_t head_ = 0;
    size_t size_ = 0;
    size_t grow_events_ = 0;
};

struct WindowResult {
    Price mean;    // rounded half-up to a cent
    Price median;  // an even-count half cent rounds up
    Price low;
    Price high;
    size_t count;
};

template <WindowPolicy PolicyT, typename Tracer = NullTracer>
class SlidingWindowStats {
  public:
    struct Sample {
        int64_t ts_ns;
        typename PolicyT::Handle handle;
    };

    // capacity_hint: initial ring size and policy reservation.
    SlidingWindowStats(int64_t window_ns, size_t capacity_hint, Tracer& tracer)
        : window_ns_(window_ns),
          ring_(capacity_hint),
          tracer_(tracer),
          policy_(capacity_hint == 0 ? 1 : capacity_hint) {
        assert(window_ns > 0);
    }

    // Not copyable: ring Handles refer into policy_.
    SlidingWindowStats(const SlidingWindowStats&) = delete;
    SlidingWindowStats& operator=(const SlidingWindowStats&) = delete;

    // For callers with no 24h band data.
    WindowResult on_tick(int64_t event_ts_ns, Price price) noexcept {
        return on_tick(event_ts_ns, price, no_band);
    }

    // Adds one sample and returns the updated stats: clamp timestamp, evict
    // expired samples, insert, query. This is the per-tick hot path.
    //
    // `band` returns {low_24h, high_24h} as optional<Price>. It is called only
    // if the policy supports note_market() and still wants_market(), so the
    // caller can parse the band lazily (at most once per symbol).
    template <typename BandFn>
    WindowResult on_tick(int64_t event_ts_ns, Price price, BandFn&& band) noexcept {
        if (event_ts_ns < last_ts_) {
            event_ts_ns = last_ts_;  // clamp: out-of-order event time never rewinds the window
        } else {
            last_ts_ = event_ts_ns;
        }

        {
            auto scope = tracer_.enter(ProbeId::WinEvict);
            const int64_t cutoff = event_ts_ns - window_ns_;
            uint64_t evicted = 0;
            while (!ring_.empty() && ring_.front().ts_ns <= cutoff) {
                Sample& s = ring_.front();
                sum_ -= policy_.value_of(s.handle);
                policy_.erase(s.handle);
                ring_.pop_front();
                ++evicted;
            }
            // A count, so recorded under its own probe, not the duration probe.
            tracer_.note(ProbeId::WinEvictCount, evicted);
        }

        // Compiled only for policies that declare note_market()/wants_market().
        if constexpr (requires { policy_.note_market(Price{}, Price{}); policy_.wants_market(); }) {
            if (policy_.wants_market()) {
                auto [low_24h, high_24h] = band();
                if (low_24h && high_24h) {
                    policy_.note_market(*low_24h, *high_24h);
                }
            }
        }

        // If the price falls outside the policy's band, rebuild the band first.
        // Lives here, not in the policy, because it needs the live ring.
        if constexpr (requires { policy_.would_escape(price); policy_.rebuild_market(Price{}, Price{}); }) {
            if (policy_.would_escape(price)) {
                rebuild_policy_for_escape(price);
            }
        }

        {
            auto scope = tracer_.enter(ProbeId::WinInsert);
            auto h = policy_.insert(price);
            ring_.push_back(Sample{event_ts_ns, h});
            sum_ += price;
        }

        WindowResult r;
        {
            auto scope = tracer_.enter(ProbeId::WinQuery);
            r.count = policy_.size();
            r.mean = round_mean();
            r.median = policy_.median();
            r.low = policy_.low();
            r.high = policy_.high();
        }
        return r;
    }

    size_t size() const noexcept { return policy_.size(); }
    size_t ring_grow_events() const noexcept { return ring_.grow_events(); }

    // Access to policy-specific diagnostics (e.g. fallback_count()). Non-const
    // because some policies' accessors are non-const.
    PolicyT& policy() noexcept { return policy_; }

    // Empties the window but keeps allocated capacity; used by the benchmark
    // to replay a capture several times without re-paying first-fill costs.
    void reset() noexcept {
        ring_.reset();
        policy_.clear();
        sum_ = 0;
        last_ts_ = INT64_MIN;
    }

  private:
    static std::pair<std::optional<Price>, std::optional<Price>> no_band() noexcept { return {std::nullopt, std::nullopt}; }

    // Mean in cents, rounded half-up (error at most half a cent). sum_ is a
    // uint64 of uint32 cents, so it cannot overflow below ~4e9 samples.
    Price round_mean() const noexcept {
        const size_t n = policy_.size();
        assert(n > 0);
        [[assume(n > 0)]];  // assert() is compiled out in Release; this still informs the optimizer
        return static_cast<Price>((sum_ + n / 2) / n);
    }

    // Widens the band to cover every live price plus `price`, then re-inserts
    // every live sample in FIFO order, replacing its Handle (old Handles point
    // into the discarded histogram). O(live samples); rare in practice.
    void rebuild_policy_for_escape(Price price) noexcept {
        const Price new_low = ring_.empty() ? price : std::min(policy_.low(), price);
        const Price new_high = ring_.empty() ? price : std::max(policy_.high(), price);
        policy_.rebuild_market(new_low, new_high);
        for (size_t i = 0; i < ring_.size(); ++i) {
            Sample& s = ring_.at(i);
            const Price p = policy_.value_of(s.handle);  // Handle stores its price, so still readable
            s.handle = policy_.insert(p);
        }
    }

    // Hot per-tick fields first so they share cache lines; policy_ (can be
    // large) last.
    uint64_t sum_ = 0;
    int64_t window_ns_;
    int64_t last_ts_ = INT64_MIN;
    Ring<Sample> ring_;
    Tracer& tracer_;
    PolicyT policy_;
};

}  // namespace ticker
