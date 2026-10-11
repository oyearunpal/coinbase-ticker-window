#pragma once
// One sorted array of every live price (detail::SortedPriceRun). Bit-exact on all four stats
// and the direct competitor to ExactMapWindow: same answers, different ordered structure.
//   insert/erase  O(log n) locate + one memmove     median/low/high  O(1)
// Memory is 4 bytes per live sample, reserved up to capacity_hint.
#include <cassert>
#include <cstddef>

#include "../fixed_price.hpp"
#include "sorted_price_run.hpp"

namespace ticker {

class SortedVectorWindow {
  public:
    // Price only: erase() re-locates by value.
    struct Handle {
        Price price;
    };

    explicit SortedVectorWindow(size_t capacity_hint) : run_(capacity_hint) {}

    Handle insert(Price price) {
        run_.insert(price);
        return Handle{price};
    }

    void erase(Handle h) noexcept {
        const bool removed = run_.erase(h.price);
        assert(removed && "erase() of a price not in the run -- Handle/window mismatch");
        (void)removed;
    }

    Price value_of(Handle h) const noexcept { return h.price; }

    // Lower middle for odd n, mean of the two middles for even n; same rule as ExactMapWindow
    // and test::ReferenceWindow.
    Price median() const noexcept {
        const size_t n = run_.size();
        assert(n > 0);
        if (n % 2 == 1) return run_[n / 2];
        return detail::avg_price(run_[n / 2 - 1], run_[n / 2]);
    }

    Price low() const noexcept {
        assert(!run_.empty());
        return run_.front();
    }

    Price high() const noexcept {
        assert(!run_.empty());
        return run_.back();
    }

    size_t size() const noexcept { return run_.size(); }
    bool empty() const noexcept { return run_.empty(); }

    void clear() noexcept { run_.clear(); }

    // Diagnostic, not part of the WindowPolicy concept.
    size_t policy_bytes() const noexcept { return run_.bytes(); }

  private:
    detail::SortedPriceRun run_;
};

static_assert(sizeof(SortedVectorWindow::Handle) == 4, "price only -- no bucket index, no iterator");

}  // namespace ticker
