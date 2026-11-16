#!/usr/bin/env python3
"""Generates tests/data/<scenario>/{events.csv,queries.csv} from a readable spec.

Timestamps are written as nanoseconds since midnight (ITCH convention) with the
human-readable time kept in a trailing comment column-free line above each block.
"""
import pathlib
import sys

NS_PER_MIN = 60_000_000_000


def t(hhmmss: str) -> int:
    """'HH:MM:SS' or 'HH:MM:SS.nnnnnnnnn' -> ns since midnight."""
    if "." in hhmmss:
        base, frac = hhmmss.split(".")
        frac_ns = int(frac.ljust(9, "0"))
    else:
        base, frac_ns = hhmmss, 0
    h, m, s = (int(x) for x in base.split(":"))
    return ((h * 3600 + m * 60 + s) * 1_000_000_000) + frac_ns


# (type, time, price, shares, match, printable)
NO_BREAK_EVENTS = [
    ("E", "09:30:05", 1000000, 100, 1, "Y"),
    ("E", "09:30:20", 1001000, 200, 2, "Y"),
    ("P", "09:30:45",  999500,  50, 3, "Y"),
    ("E", "09:31:10", 1002000, 300, 4, "Y"),
    ("C", "09:31:40", 1005000, 150, 5, "N"),   # non-printable: excluded from every metric
    ("C", "09:31:50", 1003000, 120, 6, "Y"),
    # 09:32 has no trades
    ("E", "09:33:15", 1001500,  80, 7, "Y"),
    ("E", "09:33:16", 1001500,  20, 7, "Y"),   # repeated match number: VWAP counts both, RV once
    ("Q", "09:33:30", 1000500, 1000, 8, "Y"),
    ("E", "09:34:05", 1000500,  60, 9, "Y"),   # same price as previous print: TWAP step unchanged, RV return 0
    ("E", "09:34:50",  998000, 500, 10, "Y"),
    # 09:35 and 09:36 have no trades
    ("P", "09:37:25", 1004000, 250, 11, "Y"),
    ("E", "09:38:05", 1006000,  10, 12, "Y"),
    ("E", "09:38:59.999", 1005500, 40, 13, "Y"),
    ("E", "09:39:30", 1005500,  40, 14, "Y"),  # same price as previous print
]

BROKEN_TRADE_EVENTS = [
    ("E", "09:30:05", 1000000, 100, 1, "Y"),
    ("E", "09:30:20", 1001000, 200, 2, "Y"),
    ("E", "09:30:40", 1002000, 300, 3, "Y"),
    ("B", "09:30:50", None, None, 2, None),    # break a trade in the middle of the RV path
    ("E", "09:31:10", 1003000, 150, 4, "Y"),
    ("E", "09:31:20", 1003000,  50, 4, "Y"),   # repeated match number
    ("B", "09:31:30", None, None, 4, None),    # break removes both executions of match 4
    ("B", "09:31:45", None, None, 999, None),  # unknown match number: no-op
    ("E", "09:32:10", 1004000, 120, 5, "Y"),
    ("B", "09:32:30", None, None, 1, None),    # break the head of the RV path
    ("E", "09:33:10", 1005000,  70, 6, "Y"),
    ("B", "09:33:20", None, None, 6, None),    # break the tail of the RV path ...
    ("E", "09:33:40", 1006000,  90, 7, "Y"),   # ... then trade again
    ("E", "09:34:10", 1007000,  30, 8, "Y"),
    ("B", "09:36:10", None, None, 8, None),    # break two minutes after the trade
    ("E", "09:36:30", 1007500,  25, 9, "Y"),
]

METRICS = ["VWAP", "TWAP", "RV"]


def boundaries(first: str, last: str):
    a, b = t(first), t(last)
    assert a % NS_PER_MIN == 0 and b % NS_PER_MIN == 0
    return list(range(a, b + 1, NS_PER_MIN))


SCENARIOS = {
    "no_breaks_dense_queries": (NO_BREAK_EVENTS, boundaries("09:31:00", "09:41:00"), [1, 3, 5]),
    "no_breaks_sparse_queries": (NO_BREAK_EVENTS, [t("09:34:00"), t("09:36:00"), t("09:41:00")], [2, 4, 10]),
    "broken_trades": (BROKEN_TRADE_EVENTS, boundaries("09:31:00", "09:38:00"), [1, 2, 8]),
}


def fmt_time(ns: int) -> str:
    s, frac = divmod(ns, 1_000_000_000)
    h, rem = divmod(s, 3600)
    m, s = divmod(rem, 60)
    return f"{h:02d}:{m:02d}:{s:02d}.{frac:09d}"


def write_scenario(root: pathlib.Path, name: str, events, nows, minutes_list):
    d = root / name
    d.mkdir(parents=True, exist_ok=True)

    lines = [
        "# ITCH-style trade stream for one stock. Timestamps are ns since midnight,",
        "# prices have 4 implied decimal places (1000000 = $100.00).",
        "# type: E/P/Q = printable execution, C = execution with explicit printable flag, B = broken trade",
        "type,timestamp_ns,price,shares,match_number,printable",
    ]
    last_ts = -1
    for typ, when, price, shares, match, printable in events:
        ts = t(when)
        assert ts >= last_ts, when
        last_ts = ts
        if typ == "B":
            lines.append(f"# {fmt_time(ts)} break match {match}")
            lines.append(f"B,{ts},,,{match},")
        else:
            lines.append(f"# {fmt_time(ts)} {typ} {shares} @ {price / 10_000:.4f} match {match} printable {printable}")
            lines.append(f"{typ},{ts},{price},{shares},{match},{printable}")
    (d / "events.csv").write_text("\n".join(lines) + "\n")

    qlines = [
        "# Queries, sorted by now_ns. A query at now_ns observes every event with timestamp_ns < now_ns.",
        "metric,now_ns,minutes",
    ]
    for now in nows:
        qlines.append(f"# now = {fmt_time(now)}")
        for minutes in minutes_list:
            for metric in METRICS:
                qlines.append(f"{metric},{now},{minutes}")
    (d / "queries.csv").write_text("\n".join(qlines) + "\n")


if __name__ == "__main__":
    root = pathlib.Path(sys.argv[1])
    for name, (events, nows, minutes_list) in SCENARIOS.items():
        write_scenario(root, name, events, nows, minutes_list)
        print("wrote", root / name)
