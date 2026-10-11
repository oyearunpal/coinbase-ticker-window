# Coinbase ticker sliding-window stats

A C++23 process subscribes to Coinbase's public `ticker` WebSocket for several symbols. For every
trade it updates that symbol's sliding window (default 5 minutes; 1 hour or any other length is
just a config value) and appends the trade and the window's **mean, median, high, low and range**
to a per-symbol CSV. No API key is needed.

The feed thread does no allocation, locking, or file I/O in steady state. A second thread writes
the CSVs. `hybrid_histogram` is the default because it is the measured balance of speed, memory,
and accuracy: feed-path p50 is 240 ns, within about 20 ns of the fastest approximate policy, and
it stays exact for a normal 5-minute BTC/ETH/SOL window. The other policies implement the same
interface, so that comparison is measured rather than assumed, and `window.policy` still selects
any of them. Numbers are in [docs/benchmarks.md](docs/benchmarks.md). The design is in
[docs/DESIGN.md](docs/DESIGN.md).

- Mean, high and low are exact for every policy.
- **Verdict: `hybrid_histogram` is the default median policy, at any window length.** It is the
  only policy that is near the fastest, near the smallest and exact in normal use, all at once:
  - **Speed:** feed_total p50 240 ns and p99 360–470 ns, within ~20 ns of the fastest policy
    (`tracked_histogram`, which is approximate). On a 1-hour window, `sorted_vector`'s p99 reaches
    4.7 µs.
  - **Memory:** at most 256 KB per symbol for the histogram (a 65,536-cent core), against 1–2 MB
    for `range_histogram` on BTC. With 155 symbols: 21 MB peak, against 33 MB.
  - **Accuracy:**
    - **Exact** while the window's live price span (max − min) is within $655.33. That held on
      every tick of BTC/ETH/SOL at 5 minutes over a 3-hour capture.
    - **Wider span** (e.g. 1 hour on BTC): core buckets widen to ⌈(span + 3) / 65,536⌉ cents and
      the median is off by at most half a bucket, about span / 131,072. Even a 10% hourly move on
      BTC ($8,000 span) keeps the error at 6 cents (≈ 0.00008%). Measured on the 1-hour capture:
      54% of ticks exact, the rest within 0.000012% (≈ 1 cent).
    - **No usable 24h band** (rare symbols): falls back to log buckets, bounded by 1/127 ≈ 0.79%.

  See [docs/DESIGN.md](docs/DESIGN.md#accuracy) and [docs/benchmarks.md](docs/benchmarks.md).
- The feed path does no allocation, locking or file I/O in steady state. A second thread writes
  the CSVs. See [docs/DESIGN.md](docs/DESIGN.md).

## Build

Linux, CMake ≥ 3.16, a C++23 compiler (developed and tested with GCC 16), Boost ≥ 1.74 (header-only
Beast/Asio) and OpenSSL.

```bash
# Ubuntu / Debian
sudo apt install build-essential cmake libboost-dev libssl-dev python3

cmake -B build -DCMAKE_BUILD_TYPE=Release     # Release = -O3 -DNDEBUG (the default)
cmake --build build -j
```

It builds four binaries:

| binary | purpose |
|---|---|
| `ticker_feed` | the app |
| `ticker_tests` | unit tests |
| `ticker_bench` | latency/memory benchmark over a recorded capture |
| `compare_policies` | median accuracy of every policy against the exact one, over a capture |

## Run

```bash
./build/ticker_feed                      # uses ./ticker.conf: BTC-USD, ETH-USD, SOL-USD, 300s, 10 min
./build/ticker_feed --window.length=3600s --feed.duration=0   # 1-hour window, run until Ctrl-C
```

Every run writes into its own directory, `logs/<YYYYMMDD_HHMMSS>_<pid>/` (local time), and `logs/latest` links to the newest:
- `stats_<SYMBOL>.csv`: one row per trade.
- `run.log`: the resolved config, the CPU pinning and a shutdown summary (`elapsed_ms`, `parse_failures`,
  `non_ticker_frames`, `unknown_symbol`, `ring_full_spin_events`, `active_symbols`).

### Configuration

[`ticker.conf`](ticker.conf) is a small TOML-like file. Every key can be overridden on the
command line as `--section.key=value`, and unknown keys are rejected.

| key | default | meaning |
|---|---|---|
| `feed.symbols` | `["BTC-USD","ETH-USD","SOL-USD"]` | products to subscribe to (whole-cent products only) |
| `feed.duration` | `600s` | run time; `0` = until SIGINT |
| `feed.endpoint` | `wss://ws-feed.exchange.coinbase.com` | WebSocket endpoint |
| `window.length` | `300s` | sliding window length |
| `window.policy` | `hybrid_histogram` | median algorithm, at any window length (see [docs/policies.md](docs/policies.md)) |
| `output.dir` | `logs` | parent of the per-run directories that hold the CSVs and `run.log` |
| `output.flush_interval` | `200ms` | how often the writer flushes |
| `cpu.pin` | `shared_l3` | pin the two threads: `shared_l3`, `smt_siblings`, `explicit`, `none` |
| `capture.record` / `capture.replay` | `""` | record raw frames to a file, or replay one instead of connecting |

### CSV format

```
recv_ts,event_ts,trade_id,price,mean,median,high,low,range,count
2026-09-21T14:40:49.110272736Z,2026-09-21T14:40:49.029999000Z,1096125383,85627.26,85626.19,85626.19,85627.26,85625.12,2.14,2
```

- `recv_ts` is when the frame arrived (local wall clock). `event_ts` is Coinbase's trade time,
  which is what the window uses.
- Each row's stats already include that row's trade.
- `range = high - low`, and `count` is the number of trades in the window.
- Prices are in cents, printed with two decimals. Mean and an even-count median round half-up to
  the cent.

## Tests and verification

```bash
./build/ticker_tests                                          # unit tests (doctest)
tools/verify_run.sh                                              # checks the latest run's log and re-derives every CSV row
```

- **Unit tests.** Every median policy is checked against a brute-force reference: exact equality,
  or within the proven bound for the approximate ones. The checks run at 300s and 3600s, with
  duplicate-heavy and wide price streams, random-order erase and out-of-order timestamps. The
  suite also covers the parser (real Coinbase frames), CSV formatting, the SPSC ring and config
  handling. `tests/test_policy_contract.cpp` holds the shared suite.
- **`verify_run.sh`** checks one run directory (default `logs/latest`). From `run.log` it takes the window length and
  requires `parse_failures=0`, `unknown_symbol=0`, every symbol active, no feed error frames, and as
  many CSV rows as the writer logged. It then runs **`verify_csv.py`**, which recomputes every row
  from the CSV's own timestamps and prices, in Python, with no code shared with the C++.
  mean/high/low/range/count must match exactly. The median is reported as accuracy: the share of
  rows that are exact and the share in each relative-error band, measured as in
  `bench/compare_policies.cpp`. A median off by more than the proven bound (1/127 ≈ 0.787%) fails;
  otherwise a closing note says whether every median is exact or how many are approximate.

**A sample run** is in
[`logs/verification/`](logs/verification/):
- the unit-test output;
- a 10-minute live run (`run.log` + the three CSVs, 4,154 rows);
- `verify_run.log`, `verify_run.sh`'s report on that run.

## Documentation

| | |
|---|---|
| [docs/DESIGN.md](docs/DESIGN.md) | threads and data flow, prices, window semantics, the policy interface, accuracy, assumptions |
| [docs/policies.md](docs/policies.md) | how each median policy works (with diagrams), and future scaling |
| [docs/benchmarks.md](docs/benchmarks.md) | latency, memory and accuracy per policy, and how to reproduce them |

## Out of scope

There is no reconnect/backoff, sequence-gap detection, heartbeat or stale-feed watchdog. A dropped
connection ends the run. One WebSocket connection carries all symbols.
