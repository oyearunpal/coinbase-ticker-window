#pragma once
// Maps a product_id (e.g. "BTC-USD") to a dense slot index, so per-symbol
// state lives in arrays instead of a string-keyed map.
//
// Open-addressed hash table (power-of-two size, load factor <= 0.5, linear
// probing) keyed on the product_id's first 8 bytes, with a full string
// compare on a prefix match. entries_ holds the names in slot order.
#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace ticker {

class SymbolRegistry {
  public:
    static constexpr int kNotFound = -1;

    SymbolRegistry() = default;

    // Pre-sizes for max_symbols so find() never runs during a rehash.
    // Adding more still works (it rehashes).
    explicit SymbolRegistry(size_t max_symbols) { reserve(max_symbols); }

    void reserve(size_t max_symbols) {
        // Load factor <= 0.5.
        rehash_if_smaller(max_symbols * 2);
    }

    // Returns the new slot index (0-based, registration order).
    int add(std::string_view product_id) {
        const int slot = static_cast<int>(entries_.size());
        entries_.push_back(Entry{std::string(product_id)});
        // Grow when load factor would exceed 0.5 (also does the first allocation).
        if (entries_.size() > table_.size() / 2) {
            rehash_if_smaller(table_.empty() ? kMinTableSize : table_.size() * 2);
        }
        insert_into_table(prefix_word_of(product_id), slot);
        return slot;
    }

    // Hot path: hash the 8-byte prefix, linear-probe comparing prefixes, and
    // confirm with a full compare (two names can share a prefix).
    int find(std::string_view product_id) const noexcept {
        if (table_.empty()) return kNotFound;
        const uint64_t prefix = prefix_word_of(product_id);
        const size_t mask = table_.size() - 1;
        size_t idx = hash_prefix(prefix) & mask;
        for (;;) {
            const Slot& s = table_[idx];
            if (s.slot_index == kNotFound) return kNotFound;  // empty slot: probe chain ends here
            if (s.prefix == prefix) {
                const Entry& e = entries_[static_cast<size_t>(s.slot_index)];
                if (e.name.size() == product_id.size() &&
                    std::memcmp(e.name.data(), product_id.data(), product_id.size()) == 0) {
                    return s.slot_index;
                }
            }
            idx = (idx + 1) & mask;
        }
    }

    size_t size() const noexcept { return entries_.size(); }
    const std::string& name_of(int slot) const { return entries_[static_cast<size_t>(slot)].name; }

  private:
    struct Entry {
        std::string name;
    };

    // Prefix word (compared before touching the string) and slot index, or
    // kNotFound if empty.
    struct Slot {
        uint64_t prefix = 0;
        int slot_index = kNotFound;
    };

    static constexpr size_t kMinTableSize = 16;  // power of two
    static constexpr size_t kPrefixBytes = sizeof(uint64_t);

    // First 8 bytes, zero-padded, as one word. memcpy avoids aliasing UB and
    // compiles to a single load.
    static uint64_t prefix_word_of(std::string_view s) noexcept {
        char buf[kPrefixBytes] = {};
        std::memcpy(buf, s.data(), std::min(kPrefixBytes, s.size()));
        uint64_t w;
        std::memcpy(&w, buf, kPrefixBytes);
        return w;
    }

    // MurmurHash3's 64-bit finalizer (fmix64); shifts and multipliers are
    // its published constants.
    static uint64_t hash_prefix(uint64_t x) noexcept {
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdULL;
        x ^= x >> 33;
        x *= 0xc4ceb9fe1a85ec53ULL;
        x ^= x >> 33;
        return x;
    }

    // Rehashes only if the requested capacity is larger; never shrinks.
    void rehash_if_smaller(size_t min_capacity) {
        const size_t new_capacity = std::bit_ceil(std::max(min_capacity, kMinTableSize));
        if (new_capacity <= table_.size()) return;
        rehash(new_capacity);
    }

    // Cold path: startup, or growth past the reserved size.
    void rehash(size_t new_capacity) {
        std::vector<Slot> new_table(new_capacity);
        const size_t mask = new_capacity - 1;
        for (size_t i = 0; i < entries_.size(); ++i) {
            const uint64_t prefix = prefix_word_of(entries_[i].name);
            size_t idx = hash_prefix(prefix) & mask;
            while (new_table[idx].slot_index != kNotFound) idx = (idx + 1) & mask;
            new_table[idx] = Slot{prefix, static_cast<int>(i)};
        }
        table_ = std::move(new_table);
    }

    void insert_into_table(uint64_t prefix, int slot) noexcept {
        const size_t mask = table_.size() - 1;
        size_t idx = hash_prefix(prefix) & mask;
        while (table_[idx].slot_index != kNotFound) idx = (idx + 1) & mask;
        table_[idx] = Slot{prefix, slot};
    }

    std::vector<Entry> entries_;
    std::vector<Slot> table_;
};

}  // namespace ticker
