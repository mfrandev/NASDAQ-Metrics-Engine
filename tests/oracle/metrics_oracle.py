#!/usr/bin/env python3
"""Independent oracle for the engine's three metrics.

This script shares no code with the engine. It replays an ITCH-style trade
stream (events.csv) and answers "last N minutes" queries (queries.csv) straight
from the mathematical definitions, writing expected.csv for the C++ test to
compare against.

Definitions (all integers are exact; only RV uses floating point):

  Window      A query (now, N) covers [start, end) with end = now and
              start = now - N minutes. Minute boundaries are the convention the
              engine snapshots on, so queries are expected to sit on them.
  Known       A query at `now` observes every event with timestamp < now
              (ShardManager queries with the timestamp of the next message,
              before that message is processed).
  Active      A printable execution whose match number has not been broken by
              any known break message. A break removes every execution with
              that match number, as if the trade never happened.
  VWAP        sum(price*shares) / sum(shares) over active trades with
              start <= ts < end. NONE when no shares are in the window.
              Reported as the two integer sums (shares, notional).
  TWAP        The price in force at time t is the price of the last active
              trade with ts <= t. TWAP = integral(price dt) / integral(dt) over
              [max(start, first_trade_ts), end). NONE when no active trade is
              known. Reported as the two integer sums (total_time, price_time).
  RV          Sum over the RV path of (ln p_i - ln p_{i-1})^2 for trades with
              start <= ts_i < end. The path contains each active match number
              once (its first printable execution), in time order. The first
              trade on the path contributes nothing. NONE until the first
              printable trade is known (the engine emits no row before then).

Usage: metrics_oracle.py events.csv queries.csv expected.csv
"""
import csv
import math
import sys
from dataclasses import dataclass
from typing import List, Optional

NS_PER_MIN = 60_000_000_000


@dataclass
class Event:
    kind: str          # "TRADE" or "BREAK"
    ts: int
    price: int         # 4 implied decimals
    shares: int
    match: int
    printable: bool


def read_rows(path):
    with open(path, newline="") as f:
        lines = [ln for ln in f if ln.strip() and not ln.lstrip().startswith("#")]
    return list(csv.DictReader(lines))


def load_events(path) -> List[Event]:
    events = []
    for row in read_rows(path):
        typ = row["type"].strip()
        ts = int(row["timestamp_ns"])
        match = int(row["match_number"])
        if typ == "B":
            events.append(Event("BREAK", ts, 0, 0, match, False))
            continue
        price = int(row["price"])
        shares = int(row["shares"])
        if typ in ("E", "P", "Q"):
            printable = True
        elif typ == "C":
            printable = row["printable"].strip() == "Y"
        else:
            raise ValueError(f"unknown event type {typ!r}")
        if price <= 0 or shares <= 0:
            raise ValueError(f"price and shares must be positive: {row}")
        events.append(Event("TRADE", ts, price, shares, match, printable))
    for a, b in zip(events, events[1:]):
        if b.ts < a.ts:
            raise ValueError("events must be sorted by timestamp")
    return events


def active_trades(events: List[Event], now: int) -> List[Event]:
    known = [e for e in events if e.ts < now]
    broken = {e.match for e in known if e.kind == "BREAK"}
    trades = [e for e in known if e.kind == "TRADE" and e.printable and e.match not in broken]
    for a, b in zip(trades, trades[1:]):
        if b.ts == a.ts:
            raise ValueError("two printable trades share a timestamp; the TWAP step function would be ambiguous")
    return trades


def vwap(trades: List[Event], start: int, end: int):
    shares = sum(t.shares for t in trades if start <= t.ts < end)
    notional = sum(t.price * t.shares for t in trades if start <= t.ts < end)
    if shares == 0:
        return None
    return shares, notional


def twap(trades: List[Event], start: int, end: int):
    if not trades or trades[0].ts >= end:
        return None
    window_start = max(start, trades[0].ts)
    total_time = end - window_start
    if total_time <= 0:
        return None
    price_time = 0
    for i, tr in enumerate(trades):
        seg_lo = max(tr.ts, window_start)
        seg_hi = min(trades[i + 1].ts if i + 1 < len(trades) else end, end)
        if seg_hi > seg_lo:
            price_time += tr.price * (seg_hi - seg_lo)
    return total_time, price_time


def rv(events: List[Event], trades: List[Event], now: int, start: int, end: int) -> Optional[float]:
    if not any(e.kind == "TRADE" and e.printable and e.ts < now for e in events):
        return None
    seen = set()
    path = []
    for tr in trades:
        if tr.match in seen:
            continue
        seen.add(tr.match)
        path.append(tr)
    total = 0.0
    for prev, cur in zip(path, path[1:]):
        if start <= cur.ts < end:
            d = math.log(cur.price) - math.log(prev.price)
            total += d * d
    return total


def main(events_path, queries_path, expected_path):
    events = load_events(events_path)
    queries = read_rows(queries_path)

    with open(expected_path, "w", newline="") as out:
        w = csv.writer(out)
        w.writerow(["metric", "now_ns", "minutes", "status", "value_a", "value_b"])
        for q in queries:
            metric = q["metric"].strip()
            now = int(q["now_ns"])
            minutes = int(q["minutes"])
            end = now
            start = now - minutes * NS_PER_MIN
            trades = active_trades(events, now)

            if metric == "VWAP":
                r = vwap(trades, start, end)
                row = ["NONE", "", ""] if r is None else ["OK", str(r[0]), str(r[1])]
            elif metric == "TWAP":
                r = twap(trades, start, end)
                row = ["NONE", "", ""] if r is None else ["OK", str(r[0]), str(r[1])]
            elif metric == "RV":
                r = rv(events, trades, now, start, end)
                row = ["NONE", "", ""] if r is None else ["OK", repr(r), ""]
            else:
                raise ValueError(f"unknown metric {metric!r}")
            w.writerow([metric, str(now), str(minutes)] + row)


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2], sys.argv[3])
