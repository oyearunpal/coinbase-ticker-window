#include "cpu_affinity.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#endif
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace ticker {

std::vector<int> parse_cpu_list(const std::string& s) {
    std::vector<int> out;
    std::stringstream ss(s);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (part.empty()) continue;
        size_t dash = part.find('-');
        if (dash == std::string::npos) {
            out.push_back(std::atoi(part.c_str()));
        } else {
            int lo = std::atoi(part.substr(0, dash).c_str());
            int hi = std::atoi(part.substr(dash + 1).c_str());
            for (int v = lo; v <= hi; ++v) out.push_back(v);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

namespace {
std::optional<std::string> read_file(const fs::path& p) {
    std::ifstream f(p);
    if (!f) return std::nullopt;
    std::string line;
    std::getline(f, line);
    // trim trailing whitespace/newline
    while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
    return line;
}
}  // namespace

std::vector<CpuInfo> discover_topology(const std::string& sys_root) {
    std::vector<CpuInfo> out;
    fs::path cpu_dir = fs::path(sys_root) / "devices" / "system" / "cpu";
    std::error_code ec;
    if (!fs::exists(cpu_dir, ec) || !fs::is_directory(cpu_dir, ec)) return out;

    for (auto& entry : fs::directory_iterator(cpu_dir, ec)) {
        if (ec) break;
        std::string name = entry.path().filename().string();
        // Only "cpu<N>" entries (skips cpufreq, cpuidle, ...).
        constexpr std::string_view kCpuPrefix = "cpu";
        if (name.size() <= kCpuPrefix.size() || !name.starts_with(kCpuPrefix)) continue;
        bool all_digits = std::all_of(name.begin() + kCpuPrefix.size(), name.end(),
                                      [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        if (!all_digits) continue;

        int cpu_id = std::atoi(name.c_str() + kCpuPrefix.size());
        auto core_id_str = read_file(entry.path() / "topology" / "core_id");
        if (!core_id_str) continue;  // not a real CPU topology dir (e.g. cpuidle)

        CpuInfo info;
        info.cpu_id = cpu_id;
        info.core_id = std::atoi(core_id_str->c_str());
        auto l3 = read_file(entry.path() / "cache" / "index3" / "shared_cpu_list");
        if (l3) info.l3_siblings = parse_cpu_list(*l3);
        out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const CpuInfo& a, const CpuInfo& b) { return a.cpu_id < b.cpu_id; });
    return out;
}

namespace {

bool shares_l3(const CpuInfo& a, const CpuInfo& b) {
    if (a.l3_siblings.empty() || b.l3_siblings.empty()) return false;
    return std::find(a.l3_siblings.begin(), a.l3_siblings.end(), b.cpu_id) != a.l3_siblings.end();
}

PinPlan plan_shared_l3(const std::vector<CpuInfo>& topo) {
    for (size_t i = 0; i < topo.size(); ++i) {
        for (size_t j = i + 1; j < topo.size(); ++j) {
            if (topo[i].core_id != topo[j].core_id && shares_l3(topo[i], topo[j])) {
                PinPlan p;
                p.feed_core = topo[i].cpu_id;
                p.writer_core = topo[j].cpu_id;
                p.pinned = true;
                p.note = "shared_l3: cpu" + std::to_string(p.feed_core) + " + cpu" +
                          std::to_string(p.writer_core) + " (distinct physical cores, same L3 domain)";
                return p;
            }
        }
    }
    PinPlan p;
    p.note = "shared_l3: no two distinct physical cores sharing an L3 domain were found; falling back to unpinned";
    return p;
}

PinPlan plan_smt_siblings(const std::vector<CpuInfo>& topo) {
    std::map<int, std::vector<int>> by_core;
    for (auto& c : topo) by_core[c.core_id].push_back(c.cpu_id);
    for (auto& [core_id, cpus] : by_core) {
        if (cpus.size() >= 2) {
            PinPlan p;
            p.feed_core = cpus[0];
            p.writer_core = cpus[1];
            p.pinned = true;
            p.note = "smt_siblings: cpu" + std::to_string(p.feed_core) + " + cpu" +
                      std::to_string(p.writer_core) + " (hardware threads of physical core " +
                      std::to_string(core_id) + ")";
            return p;
        }
    }
    PinPlan p;
    p.note = "smt_siblings: no physical core with 2+ hardware threads was found; falling back to unpinned";
    return p;
}

}  // namespace

PinPlan choose_pin_plan(const std::vector<CpuInfo>& topo, const std::string& mode,
                         int explicit_feed_core, int explicit_writer_core) {
    if (mode == "none") {
        PinPlan p;
        p.note = "pinning disabled by config (cpu.pin = none)";
        return p;
    }
    if (mode == "explicit") {
        PinPlan p;
        p.feed_core = explicit_feed_core;
        p.writer_core = explicit_writer_core;
        p.pinned = true;
        p.note = "explicit: cpu" + std::to_string(explicit_feed_core) + " + cpu" +
                  std::to_string(explicit_writer_core) + " (from config, not validated against topology)";
        return p;
    }
    if (topo.empty()) {
        PinPlan p;
        p.note = "cpu.pin = " + mode + " requested, but topology discovery found no usable /sys tree; falling back to unpinned";
        return p;
    }
    if (mode == "shared_l3") return plan_shared_l3(topo);
    if (mode == "smt_siblings") return plan_smt_siblings(topo);

    PinPlan p;
    p.note = "unknown cpu.pin mode '" + mode + "'; falling back to unpinned";
    return p;
}

bool pin_current_thread_to(int cpu_id) noexcept {
    if (cpu_id < 0) return false;
#ifdef __linux__
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    return rc == 0;
#else
    (void)cpu_id;
    return false;  // macOS and other hosts: no pthread_setaffinity_np
#endif
}

}  // namespace ticker
