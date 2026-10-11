#include "doctest.h"
#include "../src/cpu_affinity.hpp"

#include <filesystem>
#include <fstream>

using namespace ticker;
namespace fs = std::filesystem;

namespace {

// Builds a synthetic /sys/devices/system/cpu tree under `root` from a
// simple spec: for each cpu, its core_id and its L3 shared_cpu_list string.
void write_cpu(const fs::path& root, int cpu_id, int core_id, const std::string& l3_list) {
    fs::path cpu_dir = root / "devices" / "system" / "cpu" / ("cpu" + std::to_string(cpu_id));
    fs::create_directories(cpu_dir / "topology");
    fs::create_directories(cpu_dir / "cache" / "index3");
    {
        std::ofstream f(cpu_dir / "topology" / "core_id");
        f << core_id << "\n";
    }
    {
        std::ofstream f(cpu_dir / "cache" / "index3" / "shared_cpu_list");
        f << l3_list << "\n";
    }
}

fs::path make_temp_root(const std::string& name) {
    fs::path root = fs::temp_directory_path() / ("ticker_test_sys_" + name);
    fs::remove_all(root);
    fs::create_directories(root);
    return root;
}

}  // namespace

TEST_CASE("parse_cpu_list: ranges and singles") {
    auto v = parse_cpu_list("0-2,6-8");
    CHECK(v == std::vector<int>{0, 1, 2, 6, 7, 8});
    auto v2 = parse_cpu_list("3,7,11");
    CHECK(v2 == std::vector<int>{3, 7, 11});
    auto v3 = parse_cpu_list("5");
    CHECK(v3 == std::vector<int>{5});
}

TEST_CASE("discover_topology + shared_l3: dual-CCX machine (like the dev box)") {
    // Mirrors the actual Ryzen 3600 topology used during development:
    // CCX0 = cpu 0,1,2 (+SMT 6,7,8), CCX1 = cpu 3,4,5 (+SMT 9,10,11).
    auto root = make_temp_root("dual_ccx");
    write_cpu(root, 0, 0, "0-2,6-8");
    write_cpu(root, 1, 1, "0-2,6-8");
    write_cpu(root, 2, 2, "0-2,6-8");
    write_cpu(root, 3, 4, "3-5,9-11");
    write_cpu(root, 4, 5, "3-5,9-11");
    write_cpu(root, 5, 6, "3-5,9-11");
    write_cpu(root, 6, 0, "0-2,6-8");
    write_cpu(root, 7, 1, "0-2,6-8");
    write_cpu(root, 8, 2, "0-2,6-8");
    write_cpu(root, 9, 4, "3-5,9-11");
    write_cpu(root, 10, 5, "3-5,9-11");
    write_cpu(root, 11, 6, "3-5,9-11");

    auto topo = discover_topology(root.string());
    REQUIRE(topo.size() == 12);

    auto plan = choose_pin_plan(topo, "shared_l3", -1, -1);
    CHECK(plan.pinned);
    // Must be two distinct physical cores (different core_id) in the same L3 group.
    auto find_core = [&](int cpu) {
        for (auto& c : topo) if (c.cpu_id == cpu) return c.core_id;
        return -1;
    };
    CHECK(find_core(plan.feed_core) != find_core(plan.writer_core));
    // And they must actually share an L3 domain.
    CHECK(((plan.feed_core <= 2 || (plan.feed_core >= 6 && plan.feed_core <= 8)) ==
           (plan.writer_core <= 2 || (plan.writer_core >= 6 && plan.writer_core <= 8))));

    auto smt_plan = choose_pin_plan(topo, "smt_siblings", -1, -1);
    CHECK(smt_plan.pinned);
    CHECK(find_core(smt_plan.feed_core) == find_core(smt_plan.writer_core));
    CHECK(smt_plan.feed_core != smt_plan.writer_core);

    fs::remove_all(root);
}

TEST_CASE("choose_pin_plan: single-CCX machine, shared_l3 still finds two distinct cores") {
    auto root = make_temp_root("single_ccx");
    write_cpu(root, 0, 0, "0-3");
    write_cpu(root, 1, 1, "0-3");
    write_cpu(root, 2, 2, "0-3");
    write_cpu(root, 3, 3, "0-3");
    auto topo = discover_topology(root.string());
    REQUIRE(topo.size() == 4);
    auto plan = choose_pin_plan(topo, "shared_l3", -1, -1);
    CHECK(plan.pinned);
    CHECK(plan.feed_core != plan.writer_core);
    fs::remove_all(root);
}

TEST_CASE("choose_pin_plan: single-core machine falls back to unpinned rather than refusing to run") {
    auto root = make_temp_root("single_core");
    write_cpu(root, 0, 0, "0");
    auto topo = discover_topology(root.string());
    REQUIRE(topo.size() == 1);

    auto plan = choose_pin_plan(topo, "shared_l3", -1, -1);
    CHECK_FALSE(plan.pinned);
    CHECK(!plan.note.empty());  // must explain itself

    auto smt_plan = choose_pin_plan(topo, "smt_siblings", -1, -1);
    CHECK_FALSE(smt_plan.pinned);
    fs::remove_all(root);
}

TEST_CASE("choose_pin_plan: missing /sys tree falls back to unpinned gracefully") {
    std::vector<CpuInfo> empty_topo;
    auto plan = choose_pin_plan(empty_topo, "shared_l3", -1, -1);
    CHECK_FALSE(plan.pinned);
    CHECK(!plan.note.empty());
}

TEST_CASE("choose_pin_plan: none mode never pins") {
    std::vector<CpuInfo> empty_topo;
    auto plan = choose_pin_plan(empty_topo, "none", -1, -1);
    CHECK_FALSE(plan.pinned);
}

TEST_CASE("choose_pin_plan: explicit mode trusts the config verbatim") {
    std::vector<CpuInfo> empty_topo;
    auto plan = choose_pin_plan(empty_topo, "explicit", 3, 5);
    CHECK(plan.pinned);
    CHECK(plan.feed_core == 3);
    CHECK(plan.writer_core == 5);
}

TEST_CASE("pin_current_thread_to: pinning to cpu 0 succeeds on this machine") {
    // This runs on the real host (not a synthetic tree) -- a basic sanity
    // check that the syscall wrapper itself works end to end. macOS and
    // other non-Linux hosts have no pthread_setaffinity_np; the wrapper
    // returns false there and the feed continues unpinned.
#ifdef __linux__
    CHECK(pin_current_thread_to(0));
#else
    CHECK_FALSE(pin_current_thread_to(0));
#endif
}

TEST_CASE("pin_current_thread_to: an absurd cpu id fails gracefully rather than crashing") {
    CHECK_FALSE(pin_current_thread_to(99999));
    CHECK_FALSE(pin_current_thread_to(-1));
}
