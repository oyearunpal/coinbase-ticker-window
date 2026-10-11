# Median policies

The window ([DESIGN.md](DESIGN.md#the-sliding-window)) keeps the running sum for the mean, so a
policy only has to answer **median, low and high** while trades are inserted and evicted in
arrival order. Seven policies implement that interface. The default is `hybrid_histogram` at every
window length (why: [DESIGN.md](DESIGN.md#the-windowpolicy-interface)). Measured latency, memory and
accuracy are in [benchmarks.md](benchmarks.md).

Common terms:
- **Ring entry.** Every policy pays 16 bytes per live trade in the window's ring:
  `{event_ts, Handle}`.
- **n** is the number of live trades, and **k** is the number of distinct live prices (about n/3
  to n/4 on real data).
- **Median cursor** ([`median_cursor.hpp`](../src/policy/median_cursor.hpp)). The histogram
  policies track which bucket holds the median, plus how many trades sit below it. An
  insert/erase moves the median by at most one trade, so the cursor steps at most to the next
  occupied bucket, found in O(1) through a bitmap of non-empty buckets
  ([`flat_histogram.hpp`](../src/policy/flat_histogram.hpp)).
- **Min/max queues** ([`fifo_min_max.hpp`](../src/policy/fifo_min_max.hpp)). The histogram policies
  get exact low/high from two monotonic queues, which relies on FIFO eviction.

| policy | median | insert / erase | median query | memory beyond the ring |
|---|---|---|---|---|
| `exact_map` | exact | O(log k) | O(1) amortised | ~48 B map node per distinct price |
| `sorted_vector` | exact | O(log n) + memmove | O(1) | 4 B per live trade |
| `sorted_vector_freq` | exact | O(log k) + memmove on new/vanished price | O(1) amortised | 8 B per distinct price |
| `tracked_histogram` | ≤ 0.79% | O(1) | O(1) amortised | fixed ~7 KB |
| `range_histogram` | exact in band | O(1) | O(1) amortised | 4 B per cent of the padded 24h band |
| `range_histogram_no_rebuild` | exact in band, coarse outside | O(1) | O(1) amortised | as above |
| `hybrid_histogram` | exact (normal conditions) | O(1) core, O(log 64) tail | O(1) amortised | ≤ 256 KB core + two ≤ 64-price tails |

---

## `exact_map`: map of price → count

```
 std::map<Price, count>          med_ ──┐   count_before_med_ = 5
   84000.00 ×2   84000.01 ×3   84000.02 ×4   84000.05 ×1
   └────── 5 trades below ────┘   ▲ median entry
 Handle = iterator to the trade's entry (stable while that price is live)
```

- **Idea.** Equal prices share one tree node, so the allocator only runs when a price appears or
  disappears, not on every trade.
- **How it works.**
  - Insert bumps a count; erase decrements through the stored iterator, with no lookup.
  - The median is a tracked entry plus the number of trades below it. Each call walks to the
    neighbouring entry if the median rank moved out, which is usually zero or one step.
  - Low and high are `begin()` and `rbegin()`.
- **Trade-off.** Simple, exact, and the reference every other policy is checked against. The cost
  is pointer chasing and a malloc/free whenever a new price appears.

## `sorted_vector`: one sorted array of every live price

```
 [ 84000.00 | 84000.00 | 84000.01 | 84000.01 | 84000.01 | 84000.02 | ... ]
    low ▲                            ▲ median = a[(n-1)/2]            high ▲ = back
 insert: binary search + memmove right;  erase: binary search for the price + memmove left
```

- **Idea.** A contiguous array: every query is an index, and a shift is one `memmove`, which is
  cache-friendly.
- **Trade-off.** The memmove is O(n), so it loses as the window grows (3600s on a liquid symbol).

## `sorted_vector_freq`: sorted array of (price, count)

```
 [ (84000.00,2) | (84000.01,3) | (84000.02,4) | (84000.05,1) ]
                                 ▲ med_idx_, count_before_med_ = 5
```

- **Idea.** Same as `sorted_vector`, but duplicates collapse into one entry. The memmove only
  happens when a price appears or disappears, and the array is k long instead of n.
- **How it works.** The median pointer is re-validated on every query and walked to the entry now
  covering the median rank. The walk is usually zero or one step, and it is counted.
- **Trade-off.** Wins when prices repeat heavily, which they do on real data. When every price is
  distinct, its 8-byte entries are worse than `sorted_vector`'s 4 bytes.

## `tracked_histogram`: log-linear histogram with a median cursor (approximate)

```
 cents:   0..63 │ 64..127 │ 128..255 │ ... │ 2^k .. 2^(k+1) │ ...   (27 ranges)
 buckets:  64   │   64    │    64    │     │  64 equal sub-buckets │   → 1,728 counters
                                  ┌─ cursor: bucket b, before = trades in buckets < b
 counts:  ... 0  0  3  7 [12]  5  0  2 ...      occupancy bitmap ▸ next/prev non-empty in O(1)
```

- **Idea.** Trade exactness for O(1) everything and a fixed, tiny footprint.
- **How it works.**
  - Each power-of-two price range is split into 64 buckets, so a bucket is at most 1/64 of its
    lower edge.
  - Insert and erase increment or decrement a counter and move the cursor.
  - The median is the midpoint of the cursor's bucket: at most 1/127 ≈ 0.79% relative error
    ([proof](DESIGN.md#accuracy)). The handle keeps the exact price, so mean, low and high stay
    exact.
- **Trade-off.** Tiny and indifferent to window length, but the median is approximate: up to
  0.75% (about $400) on BTC in the benchmark.

## `range_histogram`: one-cent buckets across the symbol's 24h band

```
 first ticker: low_24h ─── high_24h,  padded by max(10% of span, 0.1% of price, $1) each side
 [ 32 log tail ][ one bucket per cent: origin … origin + band ][ 32 log tail ]
                         ▲ median cursor walks here → exact
 trade outside the band ─► ask the window to rebuild: new band = live [min,max] + price, replay ring
```

- **Idea.** Coinbase's ticker carries `low_24h`/`high_24h`. Sizing the histogram to that band makes
  one bucket = one cent, which makes the histogram median exact.
- **How it works.**
  - Uses the same cursor as `tracked_histogram`.
  - A band wider than 1,000,000 cents uses multi-cent buckets.
  - A symbol with no usable band falls back to `tracked_histogram`'s buckets.
  - A price outside the band triggers a rebuild, which replays every live trade (rare on real
    data).
- **Trade-off.** Exact and O(1), but memory is proportional to the 24h band (BTC ≈ 1–2 MB per
  symbol), even though a 5-minute window only uses a small slice of it.

## `range_histogram_no_rebuild`

The same as `range_histogram`, but a price outside the band goes into a coarse log tail bucket
instead of triggering a rebuild. That removes the rare O(n) rebuild at the cost of an approximate
median for those trades.

## `hybrid_histogram`: bounded one-cent core plus exact tails

```
        left tail (sorted, ≤64)   core: 65,536 one-cent buckets = $655.36   right tail (sorted, ≤64)
 prices ─────────◄──────────────[ origin ........ cursor ........ ]──────────────►─────────
                                 ▲ centred on the first trade, sized from the 24h band (capped)
 a tail fills up ─► re-centre the core on the live [min, max] and replay the ring
 live span > core ─► buckets widen to ⌈(span+3)/65,536⌉ cents (bounded, no longer exact)
```

- **Idea.** A 5-minute window only occupies a small slice of the 24h band. Cap the one-cent core
  at 65,536 buckets (256 KB), place it around the actual data, and keep rare outliers exactly in
  two small sorted tails.
- **How it works.**
  - The median comes from the core via the cursor, or from a tail when the median falls there.
    Both are exact.
  - A re-centre (replay) only happens when a tail overflows.
- **Trade-off.** It is the default: as fast as the other histograms, exact at 5 minutes, and
  bounded memory. On long windows the live span can exceed $655.33; the buckets then widen and the
  median is off by at most half a bucket (measured ≤0.000012%; see [DESIGN.md](DESIGN.md#accuracy)).

---

## Future scaling

This section records what the current design implies beyond three symbols. None of it is built.

- **Memory per symbol**:
  - the ring: 16 B × live trades, starting at 4,096 entries (64 KB);
  - plus the policy: from ~7 KB (`tracked_histogram`) to ~256 KB (`hybrid_histogram` core) or
    about 1–2 MB (`range_histogram` on BTC);
  - plus a 32 KB CSV buffer.

  For 10,000 symbols that is about 1 GB with the ring hint alone, which mostly sits empty because
  the median symbol holds tens of trades. A per-symbol hint, taken from a recorded capture or
  resized online, would remove most of that.
- **All-liquid universe.** If many symbols trade like BTC, each one needs a large window.
  `tracked_histogram`'s fixed 7 KB then matters more than exactness, and `hybrid_histogram`'s core
  would widen on 1-hour windows.
- **Sharding across threads.** One feed thread handles all symbols today. To scale out, split
  symbols across N feed threads, each with its own WebSocket, symbol table, windows and SPSC ring.
  Windows are per-symbol, so nothing is shared between shards. One writer can drain N rings, or
  each shard can get its own writer.
- **Bursts.** The ring holds 16,384 rows. A writer stall longer than that makes the feed spin
  (counted in `run.log`). A window whose rebuild or re-centre replays n trades costs O(n) once.
- **Resiliency** (out of scope here): reconnect with backoff, sequence-gap detection and a
  stale-feed watchdog would sit in the feed thread's read loop.
