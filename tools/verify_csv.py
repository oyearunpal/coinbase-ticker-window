#!/usr/bin/env python3
"""Independently re-derives mean/median/high/low from ticker_feed CSVs'
own event_ts/price columns using a completely separate implementation
(plain Python, a sorted list, no shared code with the C++ sliding window).

mean/high/low/range/count must match exactly: every policy claims they are
exact, so any mismatch fails. The median is reported as accuracy: the share
of rows that are exact and the share in each relative-error band, with the
error measured as in bench/compare_policies.cpp. A median off by more than the
proven bound (1/127, about 0.787%) fails.

Usage: tools/verify_csv.py logs/stats_*.csv --window 300
"""
import argparse
import bisect
import csv
import datetime as dt
import sys
from collections import deque
from decimal import Decimal

HARD_KEYS = ("mean", "high", "low", "range", "count")

# The largest median relative error any policy allows (PriceBucketing's log buckets).
MEDIAN_BOUND = 1 / 127

# Upper edges of the median relative-error bands; the last is MEDIAN_BOUND.
BANDS = (1e-6, 1e-4, MEDIAN_BOUND)


def parse_ts(s: str) -> int:
    # "YYYY-MM-DDTHH:MM:SS.nnnnnnnnnZ" -> integer nanoseconds since epoch.
    assert s.endswith("Z")
    main, frac = s[:-1].split(".")
    d = dt.datetime.strptime(main, "%Y-%m-%dT%H:%M:%S").replace(tzinfo=dt.timezone.utc)
    epoch_ns = int(d.timestamp()) * 1_000_000_000
    return epoch_ns + int(frac)


def cents(s: str) -> int:
    return int(Decimal(s) * 100)


def div_half_up(num: int, den: int) -> int:
    # Prices are whole cents; the mean and an even-count median are rounded
    # half-up to a cent (src/fixed_price.hpp). Both operands are non-negative.
    return (2 * num + den) // (2 * den)


def pct(x: float) -> str:
    return f"{x * 100:.6f}".rstrip("0").rstrip(".") + "%"


class Accuracy:
    def __init__(self) -> None:
        self.rows = 0
        self.hard_mismatches = 0
        self.exact = 0
        self.bands = [0] * (len(BANDS) + 1)  # last slot: above MEDIAN_BOUND
        self.max_rel = 0.0
        self.sum_rel = 0.0
        self.max_abs = 0

    def add_median(self, logged: int, exact: int) -> None:
        diff = abs(logged - exact)
        if diff == 0:
            self.exact += 1
            return
        rel = diff / exact
        self.bands[bisect.bisect_left(BANDS, rel)] += 1
        self.max_rel = max(self.max_rel, rel)
        self.sum_rel += rel
        self.max_abs = max(self.max_abs, diff)

    @property
    def over_bound(self) -> int:
        return self.bands[-1]

    def merge(self, other: "Accuracy") -> None:
        self.rows += other.rows
        self.hard_mismatches += other.hard_mismatches
        self.exact += other.exact
        self.bands = [a + b for a, b in zip(self.bands, other.bands)]
        self.max_rel = max(self.max_rel, other.max_rel)
        self.sum_rel += other.sum_rel
        self.max_abs = max(self.max_abs, other.max_abs)

    def report(self, label: str) -> str:
        n = max(self.rows, 1)
        parts = [f"{self.exact / n:.1%} exact"]
        labels = [f"≤ {pct(b)}" for b in BANDS] + [f"> {pct(BANDS[-1])}"]
        parts += [f"{c / n:.1%} {lbl}" for c, lbl in zip(self.bands, labels) if c]
        median = ", ".join(parts)
        if self.over_bound:
            median += f" ({self.over_bound} over the {pct(MEDIAN_BOUND)} bound)"
        if self.exact < self.rows:
            median += (f" (max {pct(self.max_rel)} = {self.max_abs} cent{'' if self.max_abs == 1 else 's'}; "
                       f"mean error {pct(self.sum_rel / n)})")
        return (f"{label}: {self.rows} rows; mean/high/low/range/count mismatches: {self.hard_mismatches}\n"
                f"  median: {median}")


def verify(path: str, window_ns: int, max_rows: int) -> Accuracy:
    acc = Accuracy()
    fifo: deque[tuple[int, int]] = deque()  # (ts_ns, price_cents), arrival order
    prices: list[int] = []                  # the same prices, kept sorted
    total = 0
    last_ts = None

    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            ts = parse_ts(row["event_ts"])
            if last_ts is not None and ts < last_ts:
                ts = last_ts  # mirror the C++ side's non-decreasing clamp
            else:
                last_ts = ts
            price = cents(row["price"])
            fifo.append((ts, price))
            bisect.insort(prices, price)
            total += price

            cutoff = ts - window_ns
            while fifo[0][0] <= cutoff:
                _, old = fifo.popleft()
                del prices[bisect.bisect_left(prices, old)]
                total -= old

            n = len(prices)
            lo, hi = prices[0], prices[-1]
            median = prices[n // 2] if n % 2 == 1 else div_half_up(prices[n // 2 - 1] + prices[n // 2], 2)
            expected = {"mean": div_half_up(total, n), "high": hi, "low": lo, "range": hi - lo, "count": n}
            logged = {key: int(row[key]) if key == "count" else cents(row[key]) for key in expected}

            acc.rows += 1
            acc.add_median(cents(row["median"]), median)
            mismatch_keys = [key for key in HARD_KEYS if logged[key] != expected[key]]
            if mismatch_keys:
                acc.hard_mismatches += 1
                if acc.hard_mismatches <= 10:
                    print(f"MISMATCH {path} row {acc.rows} (keys={mismatch_keys}): logged={logged} "
                          f"expected={expected} (prices in cents)", file=sys.stderr)

            if max_rows and acc.rows >= max_rows:
                break
    return acc


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("csv_paths", nargs="+")
    ap.add_argument("--window", type=float, required=True, help="window length in seconds")
    ap.add_argument("--max-rows", type=int, default=0, help="stop after this many rows per file (0 = all)")
    args = ap.parse_args()

    window_ns = int(args.window * 1_000_000_000)
    overall = Accuracy()
    for path in args.csv_paths:
        acc = verify(path, window_ns, args.max_rows)
        print(acc.report(path))
        overall.merge(acc)
    if len(args.csv_paths) > 1:
        print(overall.report(f"all files (window={args.window:g}s)"))

    print()
    if overall.over_bound:
        print(f"FAIL: {overall.over_bound} median(s) off by more than the {pct(MEDIAN_BOUND)} bound")
    elif overall.exact < overall.rows:
        print(f"note: the median is approximate in {overall.rows - overall.exact} row(s), "
              f"all within the {pct(MEDIAN_BOUND)} bound (max {pct(overall.max_rel)})")
    else:
        print("note: every median is exact")
    return 1 if overall.hard_mismatches or overall.over_bound else 0


if __name__ == "__main__":
    sys.exit(main())
