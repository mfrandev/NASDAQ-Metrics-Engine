# Metric tests

Tests for VWAP, TWAP and Realized Variance. The parser and order book are out of scope: trades are fed straight to the per-stock metric state (`PerStockState`) in the same call order `ShardConsumer::processMessage` uses, and queries go through the same guards `ShardManager` applies before it writes a row.

```sh
./buildAndRunTests.sh      # or ./buildTests.sh followed by ./runTests.sh
```

`buildTests.sh` configures `./build` (without wiping it) and builds the `metrics_tests` target; `runTests.sh` runs `ctest --output-on-failure` in `./build`. The equivalent manual commands:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release   # BUILD_TESTING is ON by default
cmake --build build --target metrics_tests
cd build && ctest --output-on-failure
```

To run one piece directly: `build/tests/metrics_tests unit`, or `build/tests/metrics_tests oracle <events.csv> <queries.csv> <expected.csv>` with an `expected.csv` produced by the oracle. Pass `-DBUILD_TESTING=OFF` to CMake to skip the tests. Python 3 is needed for the oracle scenarios; without it only the unit tests are registered.

## What is tested

| ctest name | What it does |
|---|---|
| `metrics_unit_tests` | Hand-computed expectations for the accumulators (`accumulateVWAP`, `accumulateTWAP`, `accumulateRV`), the corrections, the ledger's broken-trade bookkeeping, and windowed queries through the prefix histories. |
| `metrics_oracle_<scenario>` | Replays `tests/data/<scenario>/events.csv`, answers `queries.csv` with the engine, and compares each answer with `expected.csv` produced by `tests/oracle/metrics_oracle.py`. |

Scenarios:

- `no_breaks_dense_queries`: 15 trades over 10 minutes, including an empty minute, a non-printable execution, a repeated match number, and same-price prints. Queries on every minute boundary.
- `no_breaks_sparse_queries`: the same trades with queries on only three boundaries, so most minute snapshots are created lazily by the first trade after the boundary.
- `broken_trades`: breaks of a middle, head and tail trade of the RV path, a repeated match number, an unknown match number, and a break two minutes after its trade.

`tests/data/generate_scenarios.py` regenerates the data files from a readable spec.

## The oracle

`metrics_oracle.py` shares no code with the engine and works from the definitions:

- A query `(now, N)` covers `[now - N min, now)`. A query at `now` sees every event with `timestamp < now`, which is how `ShardManager::dispatch` queries (with the timestamp of the next message, before that message is processed).
- A broken trade removes every execution with that match number, as if it never happened.
- VWAP: `sum(price * shares) / sum(shares)` over trades in the window. Compared as exact integers.
- TWAP: the last traded price is held until the next trade; `integral(price dt) / integral(dt)` from the later of the window start and the first trade. Compared as exact integers.
- RV: `sum((ln p_i - ln p_{i-1})^2)` over the path of distinct match numbers, attributed to the later trade's timestamp. Compared with a tolerance of `1e-12 + 1e-9 * |expected|`.

Every expectation in the unit tests follows the same definitions. A failing test means the engine's output differs from the definition; the test output prints both values.

## Data formats

`events.csv`: `type,timestamp_ns,price,shares,match_number,printable`. `type` is an ITCH message type: `E`, `P`, `Q` (printable executions), `C` (uses the `printable` column) or `B` (broken trade; only `match_number` is set). Timestamps are nanoseconds since midnight; prices carry four implied decimals. Lines starting with `#` are comments.

`queries.csv`: `metric,now_ns,minutes`, sorted by `now_ns`. Queries should sit on minute boundaries: the engine resolves window starts to its minute snapshots.

`expected.csv` (generated): `metric,now_ns,minutes,status,value_a,value_b`. For VWAP `value_a,value_b` are `shares,notional`; for TWAP `total_time_ns,price_time`; for RV `value_a` is the sum. `status` is `NONE` when no row is expected.
