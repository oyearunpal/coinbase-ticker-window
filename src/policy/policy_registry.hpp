#pragma once
// The one place that knows every WindowPolicy this project ships and turns a runtime name (config
// string, --policy flag) into a one-time compile-time dispatch. ticker_feed and ticker_bench
// both go through dispatch_policy().
#include <array>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include "exact_map.hpp"
#include "hybrid_histogram.hpp"
#include "range_histogram.hpp"
#include "sorted_vector.hpp"
#include "sorted_vector_freq.hpp"
#include "tracked_histogram.hpp"

namespace ticker {

enum class PolicyId {
    kExactMap,
    kTrackedHistogram,
    kRangeHistogram,
    kRangeHistogramNoRebuild,
    kSortedVector,
    kSortedVectorFreq,
    kHybridHistogram,
};

// The single (name, PolicyId) table; policy_id_name() and parse_policy_id() loop over it.
inline constexpr std::array<std::pair<std::string_view, PolicyId>, 7> kPolicyNames{{
    {"exact_map", PolicyId::kExactMap},
    {"tracked_histogram", PolicyId::kTrackedHistogram},
    {"range_histogram", PolicyId::kRangeHistogram},
    {"range_histogram_no_rebuild", PolicyId::kRangeHistogramNoRebuild},
    {"sorted_vector", PolicyId::kSortedVector},
    {"sorted_vector_freq", PolicyId::kSortedVectorFreq},
    {"hybrid_histogram", PolicyId::kHybridHistogram},
}};

inline const char* policy_id_name(PolicyId id) noexcept {
    for (const auto& [name, pid] : kPolicyNames) {
        if (pid == id) return name.data();
    }
    std::unreachable();
}

// Throws std::invalid_argument on an unknown name (AppConfig::load turns it into a ConfigError).
inline PolicyId parse_policy_id(const std::string& name) {
    for (const auto& [candidate, pid] : kPolicyNames) {
        if (name == candidate) return pid;
    }
    std::string valid;
    for (const auto& entry : kPolicyNames) valid += (valid.empty() ? "" : "|") + std::string(entry.first);
    throw std::invalid_argument("window.policy must be one of " + valid + ", got: " + name);
}

// The default window.policy at every window length. On the 3-hour sweep (docs/benchmarks.md) the
// histogram policies tie on latency within the noise, and hybrid_histogram is the one with an exact
// (or, with a capped core, near-exact) median in bounded memory. Re-run bench/run_benchmarks.sh
// and update this and that doc if a policy or the parser shifts the ranking.
inline constexpr std::string_view kDefaultPolicyName = "hybrid_histogram";

// Calls f.template operator()<PolicyT>() for the policy `id` names, once at startup and never per
// tick. F is a generic lambda:
//   dispatch_policy(id, [&]<typename PolicyT>() { run_feed<PolicyT>(cfg); });
template <typename F>
auto dispatch_policy(PolicyId id, F&& f) {
    switch (id) {
        case PolicyId::kExactMap: return f.template operator()<ExactMapWindow>();
        case PolicyId::kTrackedHistogram: return f.template operator()<TrackedHistogramWindow>();
        case PolicyId::kRangeHistogram: return f.template operator()<RangeHistogramWindow>();
        case PolicyId::kRangeHistogramNoRebuild: return f.template operator()<RangeHistogramNoRebuildWindow>();
        case PolicyId::kSortedVector: return f.template operator()<SortedVectorWindow>();
        case PolicyId::kSortedVectorFreq: return f.template operator()<SortedVectorFreqWindow>();
        case PolicyId::kHybridHistogram: return f.template operator()<HybridHistogramWindow>();
    }
    std::unreachable();
}

}  // namespace ticker
