#include "doctest.h"
#include "../src/symbol_registry.hpp"

#include <map>
#include <random>
#include <string>
#include <vector>

using namespace ticker;

TEST_CASE("SymbolRegistry: registers and finds symbols by exact name") {
    SymbolRegistry reg;
    int btc = reg.add("BTC-USD");
    int eth = reg.add("ETH-USD");
    int sol = reg.add("SOL-USD");
    CHECK(btc == 0);
    CHECK(eth == 1);
    CHECK(sol == 2);
    CHECK(reg.find("BTC-USD") == btc);
    CHECK(reg.find("ETH-USD") == eth);
    CHECK(reg.find("SOL-USD") == sol);
    CHECK(reg.size() == 3);
}

TEST_CASE("SymbolRegistry: unknown symbol returns kNotFound") {
    SymbolRegistry reg;
    reg.add("BTC-USD");
    CHECK(reg.find("DOGE-USD") == SymbolRegistry::kNotFound);
}

TEST_CASE("SymbolRegistry: distinguishes symbols sharing an 8-byte prefix") {
    SymbolRegistry reg;
    // "1INCH-USD" and "1INCH-EUR" share the first 8 bytes ("1INCH-U" is 7
    // chars... use a case that actually shares exactly 8 bytes.
    int a = reg.add("ABCDEFGH-USD");
    int b = reg.add("ABCDEFGH-EUR");
    CHECK(reg.find("ABCDEFGH-USD") == a);
    CHECK(reg.find("ABCDEFGH-EUR") == b);
    CHECK(reg.find("ABCDEFGH-GBP") == SymbolRegistry::kNotFound);
}

TEST_CASE("SymbolRegistry: name_of round-trips") {
    SymbolRegistry reg;
    reg.add("BTC-USD");
    reg.add("ETH-USD");
    CHECK(reg.name_of(0) == "BTC-USD");
    CHECK(reg.name_of(1) == "ETH-USD");
}

TEST_CASE("SymbolRegistry: handles a short product_id (fewer than 8 bytes)") {
    SymbolRegistry reg;
    int slot = reg.add("SOL");
    CHECK(reg.find("SOL") == slot);
    CHECK(reg.find("SO") == SymbolRegistry::kNotFound);
    CHECK(reg.find("SOLX") == SymbolRegistry::kNotFound);
}

// -- Open-addressed table coverage (added alongside the linear-scan ->
// hash-table change; see symbol_registry.hpp's header comment) ----------

TEST_CASE("SymbolRegistry: find() on an empty (never-add()ed) registry") {
    SymbolRegistry reg;
    CHECK(reg.find("BTC-USD") == SymbolRegistry::kNotFound);
    CHECK(reg.find("") == SymbolRegistry::kNotFound);
}

TEST_CASE("SymbolRegistry: empty-string query and >8-byte query") {
    SymbolRegistry reg;
    int short_slot = reg.add("");  // degenerate but not disallowed by the API
    int long_slot = reg.add("VERYLONGPRODUCTID-USD");  // > 8 bytes, needs the full compare fallback
    CHECK(reg.find("") == short_slot);
    CHECK(reg.find("VERYLONGPRODUCTID-USD") == long_slot);
    // Shares the first 8 bytes with the long one but differs after -- must
    // not be confused with it by the prefix-only fast compare.
    CHECK(reg.find("VERYLONGPRODUCTID-EUR") == SymbolRegistry::kNotFound);
}

TEST_CASE("SymbolRegistry: reserve()/constructor(max_symbols) sizes the table without changing behavior") {
    SymbolRegistry reg(600);  // exercises the explicit-constructor path
    for (int i = 0; i < 600; ++i) {
        std::string sym = "SYM" + std::to_string(i) + "-USD";
        int slot = reg.add(sym);
        CHECK(slot == i);
    }
    for (int i = 0; i < 600; ++i) {
        std::string sym = "SYM" + std::to_string(i) + "-USD";
        CHECK(reg.find(sym) == i);
    }
    CHECK(reg.size() == 600);
}

TEST_CASE("SymbolRegistry: >= 600 symbols with no reserve() forces at least one rehash, "
          "all still find()-able afterward") {
    SymbolRegistry reg;  // no reserve() -- starts at kMinTableSize, must grow past it
    std::vector<std::string> symbols;
    symbols.reserve(600);
    for (int i = 0; i < 620; ++i) {
        symbols.push_back("REHASH" + std::to_string(i) + "-USD");
    }
    for (size_t i = 0; i < symbols.size(); ++i) {
        int slot = reg.add(symbols[i]);
        CHECK(slot == static_cast<int>(i));
    }
    CHECK(reg.size() == symbols.size());
    // Every symbol added *before* the rehash(es) that must have happened
    // along the way must still resolve to its original slot -- this is
    // the property that actually proves rehash() correctly reinserts
    // every existing entry, not just that add() doesn't crash.
    for (size_t i = 0; i < symbols.size(); ++i) {
        CHECK(reg.find(symbols[i]) == static_cast<int>(i));
    }
    CHECK(reg.find("NOT-IN-THE-REGISTRY") == SymbolRegistry::kNotFound);
}

TEST_CASE("SymbolRegistry: fuzz -- random strings (including engineered 8-byte-prefix "
          "collisions) against a std::map reference") {
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> len_d(4, 12);
    std::uniform_int_distribution<int> char_d(0, 25);
    // A handful of shared 8-byte prefixes, deliberately reused across many
    // generated strings, so the probe-chain/full-compare disambiguation
    // path (not just the common case) gets fuzzed too.
    const std::vector<std::string> shared_prefixes = {"AAAAAAAA", "BBBBBBBB", "CCCCCCCC"};

    SymbolRegistry reg;
    std::map<std::string, int> oracle;  // name -> first-assigned slot

    for (int trial = 0; trial < 5000; ++trial) {
        std::string s;
        if (trial % 3 == 0) {
            // Engineered collision: shared prefix + random suffix.
            s = shared_prefixes[static_cast<size_t>(trial) % shared_prefixes.size()];
            int suffix_len = len_d(rng);
            for (int k = 0; k < suffix_len; ++k) s += static_cast<char>('A' + char_d(rng));
        } else {
            int len = len_d(rng);
            for (int k = 0; k < len; ++k) s += static_cast<char>('A' + char_d(rng));
        }

        auto it = oracle.find(s);
        if (it == oracle.end()) {
            int slot = reg.add(s);
            oracle[s] = slot;
            CHECK(reg.find(s) == slot);  // the just-added entry, every trial (cheap)
        } else {
            CHECK(reg.find(s) == it->second);  // a repeat: still resolves to its original slot
        }
        // Every oracle entry seen so far must still resolve correctly --
        // a full sweep periodically (not every trial, which would be
        // O(trials^2)) so it spans multiple rehash boundaries as the
        // registry grows, not just the final size.
        if (trial % 100 == 0) {
            for (const auto& [name, expected_slot] : oracle) {
                CHECK(reg.find(name) == expected_slot);
            }
        }
    }
    // Final full sweep, unconditionally.
    for (const auto& [name, expected_slot] : oracle) {
        CHECK(reg.find(name) == expected_slot);
    }
    CHECK(reg.size() == oracle.size());
}
