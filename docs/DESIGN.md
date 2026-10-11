# Design

How `ticker_feed` turns a Coinbase ticker message into a CSV row. For the individual median
algorithms see [policies.md](policies.md). For measured numbers see [benchmarks.md](benchmarks.md).

## Threads and data flow

One process with two threads, pinned by default to two physical cores that share an L3 cache
([`src/cpu_affinity.hpp`](../src/cpu_affinity.hpp)):

```
 feed thread                                                                     writer thread
 ───────────                                                                     ─────────────
 WebSocket read ─► parse ticker ─► symbol → slot ─► window update ─► SPSC ring ─► format CSV row
 (Beast, TLS)     (zero-copy)      (open-addressed)  (per symbol)    (lock-free)   buffer per symbol
                                                                                   write() every 32 KiB
                                                                                   or flush_interval
```

- **Feed thread**
  - Subscribes once to the `ticker` channel for every symbol, over one WebSocket.
  - Blocks on `read`, then handles each frame to completion: parse, look up the symbol's slot,
    fold the trade into that symbol's window, and push a fixed-size `StatRecord` (trade and stats,
    no strings) onto the ring.
  - Steady state has no allocation, no lock and no file I/O.
- **Writer thread**
  - Pops records, formats each row by hand (no `printf`/iostream) into a per-symbol buffer, and
    `write()`s it when the buffer passes 32 KiB or `output.flush_interval` elapses.
  - Slow disk I/O therefore never stalls the feed.
- **SPSC ring** ([`src/spsc_ring.hpp`](../src/spsc_ring.hpp)): 16,384 slots, two cache-line-padded
  atomic indices, no CAS. If the writer ever fell 16,384 rows behind, the feed would spin; that
  shows up in `run.log` as `ring_full_spin_events`.
- **Parser** ([`src/ticker_parser.hpp`](../src/ticker_parser.hpp)): a fast scanner for Coinbase's
  field order, with a general scanner as fallback. The two must agree whenever the fast one
  succeeds, and the tests check that on real frames and on every single-byte mutation of them.
- **Symbol lookup** ([`src/symbol_registry.hpp`](../src/symbol_registry.hpp)): an open-addressed
  table keyed on the product id's first 8 bytes.
- **Tracing**: every stage is wrapped in `tracer.enter(probe)`. The app compiles with
  `NullTracer`, so the probes vanish.
  `ticker_bench` compiles the same code with `TscTracer` to time each stage.

## Prices

A price is a `uint32_t` number of **cents** ([`src/fixed_price.hpp`](../src/fixed_price.hpp)),
never a `double`:
- Integer arithmetic is exact and reproducible, so `tools/verify_csv.py` can re-derive every row
  bit-for-bit.
- Only whole-cent products are supported (quote increment 0.01: BTC-USD, ETH-USD, SOL-USD, …). A
  price with a nonzero digit past the cents fails to parse, and that message is dropped and counted.
- The maximum price is $42,949,672.95.
- The window sum is a `uint64_t`, so it cannot overflow below about 4 billion live samples.

The only rounding is on output:
- the mean is `(sum + n/2) / n`;
- an even-count median averages the two middle prices, rounding half-up.

Each is off by at most half a cent.

## The sliding window

`SlidingWindowStats<Policy>` ([`src/sliding_window.hpp`](../src/sliding_window.hpp)) owns, per
symbol:
- a ring of `{event_ts, Handle}`;
- the running sum;
- a `Policy` object that answers median, low and high.

On every trade it does three things:

1. **Clamps time.** An event timestamp older than the previous one is treated as equal to it, so
   the window never moves backwards.
2. **Evicts.** It removes every sample outside the half-open interval `(t − length, t]` (a sample
   exactly `length` old leaves), subtracting it from the sum and calling `policy.erase(handle)`.
3. **Inserts** the new trade and returns mean, median, low, high and count.

Two properties follow:
- **Event time, not arrival time.** The window depends only on Coinbase's own timestamps, so
  replaying a recorded capture reproduces the CSV byte-for-byte.
- **Window length is a runtime value.** 300s and 3600s run exactly the same code.

**Memory.** The ring starts at `kCapacityHint` = 4096 entries per symbol and doubles if a symbol
needs more; `run.log` reports this as `total_window_ring_grow_events`. A liquid symbol holds about
4.5k live trades at 300s and about 20k at 3600s, so it grows a few times at startup and never
again.

## The WindowPolicy interface

The median algorithm is a template parameter, chosen once at startup by `window.policy`
([`src/policy/policy_registry.hpp`](../src/policy/policy_registry.hpp)). There is no per-tick
virtual call. A policy implements ([`src/policy/window_policy.hpp`](../src/policy/window_policy.hpp)):

```cpp
Handle insert(Price);        // returns whatever it needs to undo this insert later
void   erase(Handle);        // called in FIFO order when the sample leaves the window
Price  value_of(Handle);     // the price a handle refers to (for the running sum)
Price  median(), low(), high();
size_t size();  bool empty();  void clear();
```

Two optional hooks are detected at compile time and cost nothing for policies without them:
- **`note_market(low_24h, high_24h)`** gives a policy the symbol's 24h band from the first ticker,
  so it can size a histogram to that symbol (range and hybrid histograms).
- **`would_escape(price)` / `rebuild_market(low, high)`** let a policy ask for a rebuild when a
  price falls outside its band. The window then re-inserts every live sample from its ring.

**The default is `hybrid_histogram` at every window length:** an exact (or, with a capped core,
near-exact) median in bounded memory, with O(1) updates. Measured latency, memory and accuracy for
all seven policies are in [benchmarks.md](benchmarks.md). Any of them can be named explicitly with
`window.policy`.

## Accuracy

Mean, high, low and count are exact for every policy. The median is either exact or within a
documented bound:

| policy | median error |
|---|---|
| `exact_map`, `sorted_vector`, `sorted_vector_freq` | exact |
| `tracked_histogram` | relative error ≤ 1/127 ≈ 0.79% (proof below) |
| `range_histogram` | exact when the symbol's padded 24h band fits in 1,000,000 cents ($10,000), which covers BTC/ETH/SOL. Otherwise, or with no band, as `tracked_histogram` |
| `range_histogram_no_rebuild` | as `range_histogram`, except a trade outside the band is counted in a coarse tail bucket instead of triggering a rebuild |
| `hybrid_histogram` | exact while the window's live price span is within $655.33 (its 65,536-cent core less 3 cents of slack); prices outside the core go to exact sorted tails. A wider span makes each core bucket `⌈(span+3)/65,536⌉` cents wide, and the error is at most half a bucket. With no 24h band, as `tracked_histogram` |

**The 1/127 bound** ([`src/policy/price_bucketing.hpp`](../src/policy/price_bucketing.hpp)):
- The log-linear buckets split every power-of-two range `[2^k, 2^(k+1))` of cents into 64 equal
  sub-buckets, so a bucket `[a, a+w)` has width `w = a/64` or less.
- The median is reported as the bucket's midpoint.
- The worst relative error is at the bottom of the bucket: `(w/2) / a ≤ 1/128`, and with integer
  rounding of the midpoint it is at most `1/(2·64 − 1) = 1/127`.
- An even-count median averages two such values, so it stays within the same bound.
- Mean, low and high never go through buckets: the handle carries the exact price, and low/high
  come from exact monotonic min/max queues ([`src/policy/fifo_min_max.hpp`](../src/policy/fifo_min_max.hpp)).

The unit tests check every policy against a brute-force median with these tolerances.
`compare_policies` measures the real deviation on recorded captures; see
[benchmarks.md](benchmarks.md).

## Assumptions

Relied on and not checked per tick (Debug builds assert most of them):

- **Whole-cent products.** `feed.symbols = ["*"]` is rejected; list products explicitly.
- **Prices fit a `uint32_t` in cents.** That is $42.9M; larger values fail to parse.
- **Coinbase's wire format.** Ticker fields are plain strings/numbers with no backslash escapes.
  An unexpected field order only costs the slower parser path.
- **Timestamps are sane.** They are clamped to be non-decreasing but not otherwise range-checked.
- **One connection.** All symbols share one WebSocket.
- **No resiliency.** A dropped socket ends the run.
