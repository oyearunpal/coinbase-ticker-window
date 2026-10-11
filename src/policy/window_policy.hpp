#pragma once
// Seam between "order statistics over the set of in-window prices" (a WindowPolicy) and "the
// sliding time window around that set" (SlidingWindowStats, sliding_window.hpp). SlidingWindowStats
// owns the ring of {timestamp, Handle}, the running sum and when to evict; a policy only has to
// undo a specific earlier insert cheaply, which is what Handle is for.
#include <concepts>
#include <cstddef>

#include "../fixed_price.hpp"

namespace ticker {

template <typename P>
concept WindowPolicy = requires(P p, Price px, typename P::Handle h) {
    typename P::Handle;
    { p.insert(px) } -> std::same_as<typename P::Handle>;
    { p.erase(h) } -> std::same_as<void>;
    { p.value_of(h) } -> std::same_as<Price>;  // price an about-to-be-evicted handle refers to
    { p.median() } -> std::same_as<Price>;
    { p.low() } -> std::same_as<Price>;
    { p.high() } -> std::same_as<Price>;
    { p.size() } -> std::same_as<size_t>;
    { p.empty() } -> std::same_as<bool>;
    { p.clear() } -> std::same_as<void>;
};

}  // namespace ticker
