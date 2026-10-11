# Benchmarks

Measured on 2026-09-30 at commit `3999a73`. Every number is the median of 3 full repeats of
`bench/run_benchmarks.sh`. The default policy is `hybrid_histogram`.

## What the numbers mean

- **`feed_total`**: time on the feed thread from receiving a tick to its row being pushed onto the
  writer queue: parse, symbol lookup, window update, building the row, queue push. It is measured
  in its own run with no other timer active, once per policy. Frames that are not ticks are not
  counted.
- **Stages**: a separate run with the default policy that times each policy-independent step on
  its own. They don't add up to `feed_total`, which also includes the window update, and each
  stage carries a few ns of its own timer cost.
  - `parse_total`: JSON → trade (counts every frame, including frames that are skipped);
  - `symbol_lookup`: product id → symbol slot;
  - `record_build`: building the row;
  - `ring_push` / `ring_pop`: handing the row to the writer thread (`ring_pop` includes polls of
    an empty queue);
  - `format_row`: formatting the CSV row on the writer thread (not written to disk here).
- **Timer resolution: 10 ns.** The CPU's cycle counter advances in steps, so a single timing is a
  multiple of 10 ns, and so are the percentiles. The mean averages hundreds of thousands of
  samples, so it resolves differences below 10 ns.
- **Peak memory**: the process's maximum resident memory while replaying the capture.
- **Median accuracy**: the largest difference between the policy's median and the exact median
  (`exact_map`) over every tick. Mean, high and low are identical to `exact_map` on every tick
  for every policy.
- **Bold**: the best value in that column.

## Setup

- **CPU:** AMD Ryzen 5 3600 (6 cores / 12 threads). Feed thread on cpu0 and writer on cpu1:
  distinct physical cores sharing an L3 (`cpu.pin = shared_l3`).
- **OS / toolchain:** Linux Mint 21.3, kernel 5.15, GCC 16, Release `-O3 -DNDEBUG`.
- **Run:** `sudo nice -n -20 sudo -u $USER logs/bench/run_3x.sh` (highest priority, as a normal
  user). Each run is a warmup pass plus 3 timed passes; 3 full repeats. The machine is a desktop,
  not a tuned benchmarking host.
- **Repeat-to-repeat variation:** the `feed_total` mean varied by 1.2% (median; at most 3.4%) and
  p99 by 1.6% (at most 5.4%). Treat differences within about 10 ns or 5% as equal.

## Captures

Raw WebSocket frames recorded with `capture.record` (not committed; too large).

| capture | products | span | frames | ticks |
|---|---|---|---:|---:|
| `three_symbols_3hr.bin` | BTC-USD, ETH-USD, SOL-USD | 3.00 h from 2026-09-22 08:42 UTC | 105,476 | 105,475 |
| `heavy_all_symbols_3hr.bin` | all online products; 155 are priced in whole cents | 3.00 h from 2026-09-22 08:42 UTC | 486,423 | 160,897 |

The feed only handles whole-cent products, so in the all-symbol capture it skips 325,525 ticker
frames for sub-cent products, plus one subscription confirmation.

## feed_total per policy

### 3 symbols, 5-minute window

105,475 ticks, 3 symbols.

| policy | mean ns | p50 ns | p90 ns | p99 ns | peak memory MB | median accuracy |
|---|---:|---:|---:|---:|---:|---|
| exact_map | 282.5 | 250 | 340 | 610 | 8.1 | reference |
| sorted_vector | 313.1 | 260 | 430 | 970 | 8.0 | **exact** |
| sorted_vector_freq | 278.5 | 230 | 340 | 710 | 8.1 | **exact** |
| tracked_histogram | **251.2** | **220** | **270** | **330** | **7.4** | ≤ 0.75% |
| range_histogram | 266.7 | 240 | 300 | 400 | 10.0 | **exact** |
| range_histogram_no_rebuild | 267.4 | 240 | 300 | 400 | 9.8 | **exact** |
| hybrid_histogram | 269.9 | 240 | 300 | 410 | 8.4 | **exact** |

### 3 symbols, 1-hour window

105,475 ticks, 3 symbols.

| policy | mean ns | p50 ns | p90 ns | p99 ns | peak memory MB | median accuracy |
|---|---:|---:|---:|---:|---:|---|
| exact_map | 279.8 | 240 | 330 | 530 | 8.5 | reference |
| sorted_vector | 738.8 | 490 | 1160 | 4719 | 9.2 | **exact** |
| sorted_vector_freq | 338.5 | 230 | 560 | 1320 | **8.3** | **exact** |
| tracked_histogram | **248.4** | **220** | **260** | **320** | 9.1 | ≤ 0.75% |
| range_histogram | 261.8 | 230 | 290 | 370 | 10.0 | **exact** |
| range_histogram_no_rebuild | 257.2 | 230 | 280 | 360 | 11.0 | **exact** |
| hybrid_histogram | 263.2 | 240 | 290 | 360 | 8.6 | 53.8% exact, 46.2% ≤ 0.000012% (widened buckets) |

### All symbols, 5-minute window

160,897 ticks, 155 symbols.

| policy | mean ns | p50 ns | p90 ns | p99 ns | peak memory MB | median accuracy |
|---|---:|---:|---:|---:|---:|---|
| exact_map | 292.0 | 270 | 390 | 720 | **17.7** | reference |
| sorted_vector | 302.2 | 260 | 430 | 960 | 18.1 | **exact** |
| sorted_vector_freq | 278.0 | 250 | 370 | 750 | 18.2 | **exact** |
| tracked_histogram | **239.2** | **230** | **290** | **360** | 18.8 | ≤ 0.78% |
| range_histogram | 262.2 | 250 | 320 | 470 | 33.1 | 95.6% exact, <0.1% ≤ 0.00001%, 4.4% ≤ 0.78% (no 24h band) |
| range_histogram_no_rebuild | 258.2 | 250 | 320 | 470 | 33.7 | 95.6% ≤ 0.57%, <0.1% ≤ 0.00001%, 4.4% ≤ 0.78% (no 24h band) |
| hybrid_histogram | 261.2 | 250 | 320 | 470 | 21.0 | 95.6% exact, 4.4% ≤ 0.78% (no 24h band) |

### All symbols, 1-hour window

160,897 ticks, 155 symbols.

| policy | mean ns | p50 ns | p90 ns | p99 ns | peak memory MB | median accuracy |
|---|---:|---:|---:|---:|---:|---|
| exact_map | 285.3 | 270 | 370 | 630 | **19.0** | reference |
| sorted_vector | 585.6 | 350 | 1010 | 3810 | 19.3 | **exact** |
| sorted_vector_freq | 309.1 | 240 | 470 | 1150 | **19.0** | **exact** |
| tracked_histogram | **235.8** | **230** | **280** | **350** | 19.6 | ≤ 0.78% |
| range_histogram | 251.8 | 240 | 300 | 430 | 34.4 | 95.6% exact, <0.1% ≤ 0.00001%, 4.4% ≤ 0.78% (no 24h band) |
| range_histogram_no_rebuild | 249.8 | 240 | 300 | 420 | 34.5 | 95.6% ≤ 0.14%, <0.1% ≤ 0.00001%, 4.4% ≤ 0.78% (no 24h band) |
| hybrid_histogram | 250.1 | 240 | 300 | 410 | 21.7 | 65.3% exact, 30.3% ≤ 0.000012% (widened buckets), 4.4% ≤ 0.78% (no 24h band) |

Accuracy detail: "exact" policies match `exact_map` on every tick.
- `hybrid_histogram` falls back to buckets when a window's live price span is wider than $655.36
  (the 1-hour rows).
- `range_histogram_no_rebuild` accepts an error when a price leaves the 24h band.
- "No 24h band" is a symbol whose `low_24h`/`high_24h` did not give a usable band; it uses
  log-scale buckets, within a proven 1/127 ≈ 0.79%.

## Stages (default policy)

All symbols, 1-hour window (160,897 ticks, 155 symbols). Times in ns.

| | parse_total | symbol_lookup | record_build | ring_push | ring_pop | format_row |
|---|---:|---:|---:|---:|---:|---:|
| mean | 132.1 | 35.9 | 16.8 | 33.6 | 31.4 | 94.6 |
| p50 | 130 | 40 | 20 | 30 | 20 | 80 |
| p90 | 160 | 40 | 20 | 50 | 60 | 150 |
| p99 | 170 | 50 | 20 | 70 | 90 | 190 |

## Reproduce

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# record captures (or point THREE_SYM_CAPTURE / ALL_SYM_CAPTURE at existing ones)
./build/ticker_feed --config=conf/capture_three_symbols_3hr.conf
bench/run_benchmarks.sh            # one sweep -> logs/bench/results.csv, logs/bench/accuracy_*.txt
bench/run_benchmarks.sh --dry-run  # print the commands only
```

For less jitter, run pinned and at high priority on an idle machine, e.g.
`sudo nice -n -20 sudo -u "$USER" bench/run_benchmarks.sh`. The tables above take the median of three
sweeps; `OUT_DIR=<dir>` writes a sweep elsewhere. The published `logs/bench/results.csv` and
`accuracy_*.txt` are the first sweep.
