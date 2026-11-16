# Metrics: VWAP, TWAP, Realized Variance, and Broken Trade Handling

This document explains how the engine computes its three per-stock metrics and how it reverses a trade that NASDAQ later breaks. The behaviour described here is checked by the tests in [tests/](tests/README.md).

All prices come from ITCH as `uint32_t` with **4 implied decimal places** (for example, `1234500` = $123.45). All timestamps are **nanoseconds since midnight**.

---

## Shared design: cumulative sums + minute-boundary snapshots

Each metric uses the same two-layer pattern, stored per stock in `PerStockState` ([ShardConsumer.hpp](src/shard/ShardConsumer.hpp)):

1. **A live accumulator** (`PerStockVWAP`, `PerStockTWAP`, `PerStockRV`). It holds *running totals since the start of the day*, which change with every trade.
2. **A prefix history** (`PerStock*PrefixHistory`). It is a `std::vector` of snapshots of those running totals, one per minute boundary. A snapshot keyed by boundary `B` holds the totals of every trade with `timestamp < B`. A vector has room for 512 snapshots reserved up front, which covers a full trading day of minutes.

Because the totals are cumulative, the value for any window `[start, end)` is the **difference of two snapshots**:

```
windowTotal = snapshot(at or before end) − snapshot(at or before start)
```

`findPrefixAtOrBeforeTimestamp` uses `std::upper_bound` to find each snapshot (O(log n)). If no snapshot exists at or before the timestamp, it returns all zeros. Every "last N minutes" query therefore costs two binary searches and a subtraction, whatever the window size. Window starts resolve to minute boundaries, which is why `ShardManager` queries right after each boundary.

For every trade the consumer first advances the buckets to the trade's timestamp (closing any boundary that has passed) and only then accumulates the trade, so a trade's contribution always lands in the minute it printed in.

A trade counts toward the metrics only if it is **printable** (`E`, `P`, `Q` always are; `C` carries a flag) and happens **during market hours**. `outside_of_market_hours` checks this against the sequence numbers of the System Event messages. The same `countedInMetrics` decision is handed to the ledger, so the ledger and the live metrics always agree on what a trade contributed.

---

## 1. VWAP: Volume-Weighted Average Price

**Files:** [PerStockVWAP.hpp](src/vwap/PerStockVWAP.hpp), [PerStockVWAPPrefixes.hpp](src/vwap/PerStockVWAPPrefixes.hpp), [VWAPQueryResults.hpp](src/vwap/VWAPQueryResults.hpp)

### Formula

$$
\text{VWAP} = \frac{\sum_i p_i \cdot q_i}{\sum_i q_i}
$$

where $p_i$ is the execution price and $q_i$ is the number of shares executed.

### Accumulation

```cpp
vwap.numShares += shares;
vwap.notional  += (__uint128_t)price * shares;   // 128-bit to avoid overflow
```

### Buckets

`processVWAPUpdate` first calls `maybeAdvanceVWAPBucket` and then accumulates. When a trade lands in a new minute, the running totals are pushed as a snapshot **keyed by the end of the bucket that just closed**, so the snapshot keyed `B` holds every trade with `timestamp < B`. Minutes without trades produce no snapshot; the previous snapshot stays valid because the totals did not change.

### Query

`queryLastNMinutesVWAP(now, N)` closes any bucket that has finished, then computes:

```
Δshares   = prefix(≤ now).numShares − prefix(≤ now − N min).numShares
Δnotional = prefix(≤ now).notional  − prefix(≤ now − N min).notional
VWAP      = round( (Δnotional / Δshares) / 10,000 , 2 dp )
```

The query returns `nullopt` (written to the output as `-1`) when `end ≤ start`, when `Δshares == 0`, or when either delta would be negative. The partial current minute is not included: VWAP is reported at minute resolution.

---

## 2. TWAP: Time-Weighted Average Price

**Files:** [PerStockTWAP.hpp](src/twap/PerStockTWAP.hpp), [PerStockTWAPPrefixes.hpp](src/twap/PerStockTWAPPrefixes.hpp), [TWAPQueryResults.hpp](src/twap/TWAPQueryResults.hpp)

### Formula

The price is treated as a **step function**: the last traded price holds until the next trade.

$$
\text{TWAP} = \frac{\sum_k p_k \cdot \Delta t_k}{\sum_k \Delta t_k}
$$

where $p_k$ is the price in force during the interval $\Delta t_k$.

### Accumulation

```cpp
// On a new price at newTimestamp:
deltaTime   = newTimestamp − lastTimestamp;
priceTime  += lastPrice * deltaTime;   // integrate the PREVIOUS price over the elapsed time
totalTime  += deltaTime;
lastPrice   = newPrice;
lastTimestamp = newTimestamp;
```

- The first trade only seeds `lastPrice` and `lastTimestamp`. No time is counted before the first print.
- An out-of-order timestamp (`newTimestamp < lastTimestamp`) is ignored.
- The consumer calls the update only when `lastTimestamp < ts && lastPrice != price`. Skipping a print at the same price loses nothing, because the step function doesn't change.

### Buckets

`maybeAdvanceTWAPBucket` loops over **every** minute boundary passed since the last update, including minutes with no trades. At each boundary it integrates `lastPrice` up to the boundary (when a price is in force) and pushes a snapshot **keyed by the boundary time itself**. A quiet minute therefore still accrues time at the carried-forward price.

### Query (as-of)

`queryLastNMinutesTWAP(now, N)` advances the buckets to `now`. It then takes a **copy** of the live accumulator and integrates that copy up to `now`, so the open partial interval since the last trade is included without changing the real state:

```
TWAP = round( (ΔpriceTime / ΔtotalTime) / 10,000 , 2 dp )
```

`ShardManager` writes a TWAP row as soon as a price is in force (`lastTimestamp != 0`), so a stock with a single print still reports that price.

---

## 3. Realized Variance (RV)

**Files:** [PerStockRealizedVariance.hpp](src/rv/PerStockRealizedVariance.hpp), [PerStockRealizedVariancePrefixes.hpp](src/rv/PerStockRealizedVariancePrefixes.hpp), [logarithm_price_cache_utils.hpp](src/utils/logarithm_price_cache_utils.hpp)

### Formula

RV is the **sum of squared log returns** between consecutive trades:

$$
\text{RV} = \sum_{i} \left( \ln p_i - \ln p_{i-1} \right)^2
$$

It is computed **trade-to-trade (tick time)**, not sampled on a fixed clock, and the output is the raw sum with no annualization.

### Accumulation

```cpp
logPrice = std::log(price * 1e-4);   // identical expression to the ledger's LogPriceCache
if (hasLastPrice) rvSum += (logPrice − cachedLastLogPrice)^2;
cachedLastLogPrice = logPrice;
```

- Prices of zero are skipped.
- **Each match number is counted only once.** The ledger returns `rvLinkCreated = true` only the first time a match number is linked into the RV path. A cross that is reported in several messages under one match number therefore adds one return, not several.
- The live accumulator and the ledger compute the log price with the same expression, so a return added live and later subtracted through a correction cancels exactly.

### Buckets and query

`maybeAdvanceRVBucket` works like TWAP's: each minute boundary that has passed produces a snapshot `{rvSum, boundaryNs}`. `queryLastNMinutesRV` returns `rvSum(end) − rvSum(start)`, using the live `rvSum` when the window ends between snapshots. A difference that is negative by a few ulps (floating-point noise left by a correction) is reported as `0`; a larger negative difference is reported as `nullopt`.

---

## 4. Broken trade handling (ITCH `B` message)

**Files:** [PerStockLedger.cpp](src/ledger/PerStockLedger.cpp), [PerStockLedgerEntry.h](src/ledger/PerStockLedgerEntry.h), [PerStockVWAPCorrection.h](src/vwap/PerStockVWAPCorrection.h), [PerStockRealizedVarianceCorrection.h](src/rv/PerStockRealizedVarianceCorrection.h), [PerStockTWAPCorrection.h](src/twap/PerStockTWAPCorrection.h)

A Broken Trade message carries only a **match number**. To undo the trade, the engine has to remember what each match number contributed and when. The **per-stock ledger** does that, and the metrics are then rewritten **as if the trade had never happened**: the correction lands in the minute the trade printed in, not the minute the break arrived in.

### The ledger

- `_perStockLedger`: a map from `matchNumber` to an index into `_entries`.
- `_entries`: a vector of `PerStockLedgerEntry` (96 bytes each) that stores:
  - `totalShares` / `totalNotionalPrice`: everything executed under this match number (used for dumps).
  - `vwapShares` / `vwapNotionalPrice`: only the portion that was **counted in the metrics** (printable, in market hours).
  - `price`, `timestampNs`: the linked trade's price and print time. Executions that share a match number share the first one's timestamp.
  - RV path fields: `logPriceScaled`, `squaredLogDiff` (the squared return from the previous linked trade *into* this one), and `previousIndex` / `nextIndex`.

The RV fields form an **intrusive doubly linked list** of counted trades in arrival order, with `lastIndex` pointing at the tail. This list is the trade path shared by RV and TWAP.

### Handling a break: `handleBrokenTradeOrOrderExecution(matchNumber)`

1. **Look up** the entry. If the match number is unknown, do nothing (`nullopt`).
2. **Unlink** the trade *b* from the path `p → b → n`, recompute *n*'s return, and build one correction per metric, each tagged with the timestamps it applies to:

   | Metric | Correction | Applied to |
   |---|---|---|
   | VWAP | `{shares, notional}` of all counted executions under the match number | live totals and every snapshot keyed after `t_b` |
   | RV | remove (x_b − x_p)², remove (x_n − x_b)², add (x_n − x_p)² | the first at snapshots after `t_b`, the other two at snapshots after `t_n`, plus the live sum |
   | TWAP | over `[t_b, t_n)` the price in force becomes `p_prev` instead of `p_b` | live integrals and every snapshot covering part of that interval |

   Edge cases:
   - *b* is the **first** trade (no *p*): *n* becomes the new head, so its RV return is 0, and for TWAP the interval `[t_b, t_n)` had no price in force at all (it is removed from `totalTime`).
   - *b* is the **last** trade (no *n*): the live RV accumulator's "last price" falls back to *p*, and TWAP's price in force falls back to `p_prev`. If *b* was the only trade, both accumulators return to their empty state.
3. **Erase** the match number from the map, so a second break of the same match number does nothing.
4. A break of a trade that was never counted (non-printable, or outside market hours) changes nothing.

In the consumer, the `B` handler advances each metric's buckets to the break's timestamp and then calls `correctHistory` on each prefix history, which rewrites the affected snapshots and the live accumulator together.

### Cost

Each break touches at most one snapshot per minute since the broken trade (≤ 390 for a full session), and breaks are rare.

---

## 5. Verification

[tests/](tests/README.md) checks everything above against an independent Python oracle that computes the three metrics straight from their definitions over ITCH-style trade streams, including repeated match numbers, non-printable executions, empty minutes, and breaks of the first, middle, last, and a two-minutes-old trade. The oracle originally exposed the following, all since fixed: VWAP snapshots keyed by bucket start (windows one minute short), RV returns attributed to the previous minute, a stale RV last price after a tail break, rounding noise turning `0` into `-1`, ledger entries for out-of-hours trades the metrics never counted, corrections landing at break time instead of trade time, and TWAP never being corrected for breaks.
