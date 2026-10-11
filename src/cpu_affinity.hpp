#pragma once
// Reads CPU topology from /sys, picks (feed_core, writer_core) for the
// requested pinning mode, and applies it with pthread_setaffinity_np.
// The /sys root is a parameter so tests can use fake topology trees.
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace ticker {

struct CpuInfo {
    int cpu_id;
    int core_id;                 // physical core id (topology/core_id)
    std::vector<int> l3_siblings;  // cache/index3/shared_cpu_list, parsed
};

struct PinPlan {
    int feed_core = -1;
    int writer_core = -1;
    bool pinned = false;   // false if the requested layout wasn't achievable
    std::string note;       // human-readable explanation, always set
};

// Parses a Linux CPU list such as "0-2,6-8" or "1,3,5".
std::vector<int> parse_cpu_list(const std::string& s);

// Topology of every CPU under `sys_root` (core_id and L3 shared_cpu_list).
// Empty if unreadable; the caller then doesn't pin.
std::vector<CpuInfo> discover_topology(const std::string& sys_root);

// `mode`: "shared_l3", "smt_siblings", "explicit" (uses the two core ids) or "none".
PinPlan choose_pin_plan(const std::vector<CpuInfo>& topo, const std::string& mode,
                         int explicit_feed_core, int explicit_writer_core);

// Pins the calling thread. Returns false (thread stays unpinned) if the OS
// refuses, e.g. a restrictive cpuset; callers log and continue.
bool pin_current_thread_to(int cpu_id) noexcept;

}  // namespace ticker
