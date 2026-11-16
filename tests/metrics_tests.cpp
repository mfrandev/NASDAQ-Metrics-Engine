// Unit tests for VWAP, TWAP and Realized Variance, plus an oracle-driven replay
// of ITCH-style trade streams. See tests/README.md.
//
//   metrics_tests unit
//   metrics_tests oracle <events.csv> <queries.csv> <expected.csv>
//
// Every expectation in this file is the mathematical definition of the metric
// (the same definitions tests/oracle/metrics_oracle.py implements), not the
// engine's current output. A failing test therefore means the engine disagrees
// with the definition.

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <ShardConsumer.hpp> // PerStockState and the per-trade recipe mirrored by Replay below
#include <logarithm_price_cache_utils.hpp>
#include <string_utils.hpp>
#include <time_utils.hpp>

#include "test_framework.hpp"

namespace {

constexpr uint64_t NS_PER_SEC = 1'000'000'000ULL;
constexpr uint64_t NS_PER_MIN = 60ULL * NS_PER_SEC;

// hh:mm:ss(.fraction) -> ns since midnight
uint64_t at(uint32_t h, uint32_t m, uint32_t s, uint64_t ns = 0) {
    return (static_cast<uint64_t>(h) * 3600ULL + m * 60ULL + s) * NS_PER_SEC + ns;
}

// ln(price) exactly as accumulateRV computes it
double lnPrice(uint32_t price) { return std::log(static_cast<double>(price)); }

// ln(price * 1e-4) exactly as the ledger's LogPriceCache computes it
double lnScaledPrice(uint32_t price) { return std::log(static_cast<double>(price) * SCALING_FACTOR); }

double sq(double x) { return x * x; }

// ---------------------------------------------------------------------------
// Replay: drives one stock's metric state exactly the way the engine does.
//
// trade()/brokenTrade() mirror ShardConsumer::processMessage for the E, C, P, Q
// and B cases (the order of the calls matters and is copied verbatim).
// The query helpers mirror ShardManager::maybeOutputQueryForLastNMinutes*.
// If ShardConsumer.hpp or ShardManager.hpp change, this struct must follow.
// ---------------------------------------------------------------------------
struct Replay {
    PerStockState state;

    Replay() { state.ledger = std::make_unique<PerStockLedger>(); }

    void trade(uint64_t ts, uint32_t price, uint32_t shares, uint64_t match, bool printable = true) {
        LedgerAddResult result = state.ledger->addExecutedTradeToLedger(printable, price, shares, match);
        if (!printable) return;
        state.vwapPrefixes.processVWAPUpdate(price, shares, ts, state.vwap);
        if (state.twap.lastTimestamp < ts && state.twap.lastPrice != price) {
            state.twapPrefixes.processTWAPUpdate(price, ts, state.twap);
        }
        if (result.rvLinkCreated) {
            accumulateRV(price, state.rv);
            state.rvPrefixes.maybeAdvanceRVBucket(ts, state.rv);
        }
    }

    void brokenTrade(uint64_t ts, uint64_t match) {
        std::optional<PerStockCorrection> correction = state.ledger->handleBrokenTradeOrOrderExecution(match);
        if (!correction.has_value()) return;
        correctVWAP(state.vwap, correction->vwapCorrection);
        correctRV(state.rv, correction->rvCorrection);
        state.vwapPrefixes.maybeAdvanceVWAPBucket(ts, state.vwap);
        state.rvPrefixes.maybeAdvanceRVBucket(ts, state.rv);
    }

    std::optional<VWAPIntervalQueryResult> vwap(uint64_t now, uint64_t minutes) {
        if (state.vwap.notional == 0) return std::nullopt; // ShardManager emits no row
        state.vwapPrefixes.maybeAdvanceVWAPBucket(now, state.vwap);
        return state.queryLastNMinutesVWAP(now, minutes);
    }

    std::optional<TWAPIntervalQueryResult> twap(uint64_t now, uint64_t minutes) {
        if (state.twap.totalTime == 0) return std::nullopt; // ShardManager emits no row
        return state.queryLastNMinutesTWAP(now, minutes);
    }

    std::optional<RVIntervalQueryResult> rv(uint64_t now, uint64_t minutes) {
        if (!state.rv.hasLastPrice) return std::nullopt; // ShardManager emits no row
        return state.queryLastNMinutesRV(now, minutes);
    }
};

// ===========================================================================
// VWAP accumulator
// ===========================================================================

TEST_CASE(vwap_accumulates_shares_and_notional) {
    PerStockVWAP vwap;
    accumulateVWAP(vwap, 1000000, 100); // 100 @ 100.00
    accumulateVWAP(vwap, 1001000, 200); // 200 @ 100.10
    CHECK_EQ(vwap.numShares, uint64_t{300});
    CHECK_EQ(vwap.notional, __uint128_t{100000000} + __uint128_t{200200000});
}

TEST_CASE(vwap_notional_uses_128_bit_arithmetic) {
    PerStockVWAP vwap;
    accumulateVWAP(vwap, UINT32_MAX, UINT64_MAX / 8); // overflows 64 bits by design
    __uint128_t expected = static_cast<__uint128_t>(UINT32_MAX) * static_cast<__uint128_t>(UINT64_MAX / 8);
    CHECK_EQ(vwap.notional, expected);
}

TEST_CASE(vwap_correction_subtracts_broken_trade) {
    PerStockVWAP vwap;
    accumulateVWAP(vwap, 1000000, 100);
    accumulateVWAP(vwap, 1001000, 200);
    correctVWAP(vwap, PerStockVWAPCorrection{200, static_cast<__uint128_t>(1001000) * 200});
    CHECK_EQ(vwap.numShares, uint64_t{100});
    CHECK_EQ(vwap.notional, __uint128_t{100000000});
}

TEST_CASE(vwap_as_double_divides_rounds_and_scales) {
    // (100*100.00 + 200*100.10) / 300 = 100.0666... -> 100.07
    VWAPIntervalQueryResult q{0, 0, 300, __uint128_t{300200000}};
    CHECK_NEAR(vwapAsDouble(q), 100.07, 1e-9);
}

// ===========================================================================
// TWAP accumulator
// ===========================================================================

TEST_CASE(twap_first_print_only_seeds_state) {
    PerStockTWAP twap;
    accumulateTWAP(twap, 1000000, 1000);
    CHECK_EQ(twap.lastPrice, uint32_t{1000000});
    CHECK_EQ(twap.lastTimestamp, uint64_t{1000});
    CHECK_EQ(twap.totalTime, uint64_t{0});
    CHECK_EQ(twap.priceTime, __uint128_t{0});
}

TEST_CASE(twap_integrates_previous_price_over_elapsed_time) {
    PerStockTWAP twap;
    accumulateTWAP(twap, 1000000, 1000);
    accumulateTWAP(twap, 1002000, 4000); // 100.00 held for 3000 ns
    accumulateTWAP(twap, 1001000, 5000); // 100.20 held for 1000 ns
    CHECK_EQ(twap.totalTime, uint64_t{4000});
    CHECK_EQ(twap.priceTime, __uint128_t{1000000} * 3000 + __uint128_t{1002000} * 1000);
    CHECK_EQ(twap.lastPrice, uint32_t{1001000});
    CHECK_EQ(twap.lastTimestamp, uint64_t{5000});
}

TEST_CASE(twap_ignores_non_monotonic_timestamp) {
    PerStockTWAP twap;
    accumulateTWAP(twap, 1000000, 1000);
    accumulateTWAP(twap, 1002000, 4000);
    accumulateTWAP(twap, 1005000, 3500); // older than lastTimestamp: ignored entirely
    CHECK_EQ(twap.totalTime, uint64_t{3000});
    CHECK_EQ(twap.priceTime, __uint128_t{1000000} * 3000);
    CHECK_EQ(twap.lastPrice, uint32_t{1002000});
    CHECK_EQ(twap.lastTimestamp, uint64_t{4000});
}

TEST_CASE(twap_as_double_divides_rounds_and_scales) {
    // 100.00 for 3000 ns and 100.20 for 1000 ns -> 100.05
    TWAPIntervalQueryResult q{0, 0, 4000, __uint128_t{1000000} * 3000 + __uint128_t{1002000} * 1000};
    CHECK_NEAR(twapAsDouble(q), 100.05, 1e-9);
    TWAPIntervalQueryResult empty{0, 0, 0, 0};
    CHECK_EQ(twapAsDouble(empty), 0.0);
}

// ===========================================================================
// Realized variance accumulator
// ===========================================================================

TEST_CASE(rv_sums_squared_log_returns) {
    PerStockRV rv;
    accumulateRV(1000000, rv);
    CHECK(rv.hasLastPrice);
    CHECK_EQ(rv.rvSum, 0.0);
    accumulateRV(1100000, rv);
    accumulateRV(1210000, rv);
    double expected = sq(lnPrice(1100000) - lnPrice(1000000)) + sq(lnPrice(1210000) - lnPrice(1100000));
    CHECK_NEAR(rv.rvSum, expected, 1e-15);
    CHECK_NEAR(rv.rvSum, 2.0 * sq(std::log(1.1)), 1e-12);
}

TEST_CASE(rv_ignores_zero_price) {
    PerStockRV rv;
    accumulateRV(0, rv);
    CHECK(!rv.hasLastPrice);
    accumulateRV(1000000, rv);
    accumulateRV(0, rv);
    CHECK_EQ(rv.rvSum, 0.0);
    CHECK_NEAR(rv.cachedLastLogPrice, lnPrice(1000000), 1e-12);
}

TEST_CASE(rv_correction_replaces_two_returns_with_one) {
    PerStockRV rv;
    rv.rvSum = 10.0;
    correctRV(rv, PerStockRVCorrection{/*oldNext*/ 3.0, /*newNext*/ 1.5, /*broken*/ 2.0});
    CHECK_NEAR(rv.rvSum, 10.0 - 3.0 + 1.5 - 2.0, 1e-15);
}

// ===========================================================================
// Ledger: match-number bookkeeping and broken-trade corrections
// ===========================================================================

TEST_CASE(ledger_counts_vwap_per_execution_and_rv_once_per_match) {
    PerStockLedger ledger;
    LedgerAddResult first = ledger.addExecutedTradeToLedger(true, 1000000, 100, 1);
    CHECK(first.vwapUpdated);
    CHECK(first.rvLinkCreated);

    LedgerAddResult repeat = ledger.addExecutedTradeToLedger(true, 1000000, 50, 1);
    CHECK(repeat.vwapUpdated);
    CHECK(!repeat.rvLinkCreated);

    LedgerAddResult nonPrintable = ledger.addExecutedTradeToLedger(false, 1005000, 150, 2);
    CHECK(!nonPrintable.vwapUpdated);
    CHECK(!nonPrintable.rvLinkCreated);

    LedgerAddResult zeroPrice = ledger.addExecutedTradeToLedger(true, 0, 10, 3);
    CHECK(!zeroPrice.vwapUpdated);
    CHECK(!zeroPrice.rvLinkCreated);

    // Breaking match 1 removes both printable executions from VWAP
    std::optional<PerStockCorrection> c = ledger.handleBrokenTradeOrOrderExecution(1);
    CHECK(c.has_value());
    CHECK_EQ(c->vwapCorrection.shares, uint64_t{150});
    CHECK_EQ(c->vwapCorrection.notional, static_cast<__uint128_t>(1000000) * 150);

    // The non-printable execution contributed nothing to VWAP
    std::optional<PerStockCorrection> c2 = ledger.handleBrokenTradeOrOrderExecution(2);
    CHECK(c2.has_value());
    CHECK_EQ(c2->vwapCorrection.shares, uint64_t{0});
    CHECK_EQ(c2->vwapCorrection.notional, __uint128_t{0});
}

TEST_CASE(ledger_unknown_or_already_broken_match_is_a_no_op) {
    PerStockLedger ledger;
    CHECK(!ledger.handleBrokenTradeOrOrderExecution(42).has_value());
    ledger.addExecutedTradeToLedger(true, 1000000, 100, 42);
    CHECK(ledger.handleBrokenTradeOrOrderExecution(42).has_value());
    CHECK(!ledger.handleBrokenTradeOrOrderExecution(42).has_value());
}

TEST_CASE(ledger_breaking_middle_trade_rebridges_rv_path) {
    const uint32_t p1 = 1000000, p2 = 1001000, p3 = 1002000;
    const double x1 = lnScaledPrice(p1), x2 = lnScaledPrice(p2), x3 = lnScaledPrice(p3);
    PerStockLedger ledger;
    ledger.addExecutedTradeToLedger(true, p1, 100, 1);
    ledger.addExecutedTradeToLedger(true, p2, 200, 2);
    ledger.addExecutedTradeToLedger(true, p3, 300, 3);

    std::optional<PerStockCorrection> c = ledger.handleBrokenTradeOrOrderExecution(2);
    CHECK(c.has_value());
    CHECK_NEAR(c->rvCorrection.brokenSquaredLogDiff, sq(x2 - x1), 1e-15);
    CHECK_NEAR(c->rvCorrection.oldNextSquaredLogDiff, sq(x3 - x2), 1e-15);
    CHECK_NEAR(c->rvCorrection.newNextSquaredLogDiff, sq(x3 - x1), 1e-15);

    // Path is now 1 -> 3. Breaking 3 (the tail) must report the re-bridged return.
    std::optional<PerStockCorrection> tail = ledger.handleBrokenTradeOrOrderExecution(3);
    CHECK(tail.has_value());
    CHECK_NEAR(tail->rvCorrection.brokenSquaredLogDiff, sq(x3 - x1), 1e-15);
    CHECK_EQ(tail->rvCorrection.oldNextSquaredLogDiff, 0.0);
    CHECK_EQ(tail->rvCorrection.newNextSquaredLogDiff, 0.0);
}

TEST_CASE(ledger_breaking_head_trade_zeroes_next_return) {
    const uint32_t p1 = 1000000, p2 = 1001000, p3 = 1002000;
    const double x1 = lnScaledPrice(p1), x2 = lnScaledPrice(p2), x3 = lnScaledPrice(p3);
    PerStockLedger ledger;
    ledger.addExecutedTradeToLedger(true, p1, 100, 1);
    ledger.addExecutedTradeToLedger(true, p2, 200, 2);
    ledger.addExecutedTradeToLedger(true, p3, 300, 3);

    std::optional<PerStockCorrection> c = ledger.handleBrokenTradeOrOrderExecution(1);
    CHECK(c.has_value());
    CHECK_EQ(c->rvCorrection.brokenSquaredLogDiff, 0.0); // head had no return of its own
    CHECK_NEAR(c->rvCorrection.oldNextSquaredLogDiff, sq(x2 - x1), 1e-15);
    CHECK_EQ(c->rvCorrection.newNextSquaredLogDiff, 0.0); // trade 2 is the new head

    // Path is now 2 -> 3; trade 3's return is unchanged.
    std::optional<PerStockCorrection> c3 = ledger.handleBrokenTradeOrOrderExecution(3);
    CHECK(c3.has_value());
    CHECK_NEAR(c3->rvCorrection.brokenSquaredLogDiff, sq(x3 - x2), 1e-15);
}

TEST_CASE(ledger_links_trade_after_tail_break_to_new_tail) {
    const uint32_t p6 = 1005000, p7 = 1006000, p8 = 1007000;
    const double x6 = lnScaledPrice(p6), x8 = lnScaledPrice(p8);
    PerStockLedger ledger;
    ledger.addExecutedTradeToLedger(true, p6, 100, 6);
    ledger.addExecutedTradeToLedger(true, p7, 100, 7);
    CHECK(ledger.handleBrokenTradeOrOrderExecution(7).has_value());
    ledger.addExecutedTradeToLedger(true, p8, 100, 8);

    // The ledger's path is 6 -> 8, so breaking 8 reports (x8 - x6)^2.
    std::optional<PerStockCorrection> c = ledger.handleBrokenTradeOrOrderExecution(8);
    CHECK(c.has_value());
    CHECK_NEAR(c->rvCorrection.brokenSquaredLogDiff, sq(x8 - x6), 1e-15);
}

// ===========================================================================
// Live RV accumulator through the consumer's call sequence
// ===========================================================================

TEST_CASE(rv_live_sum_after_breaking_middle_trade) {
    const uint32_t p1 = 1000000, p2 = 1001000, p3 = 1002000;
    Replay r;
    r.trade(at(9, 30, 5), p1, 100, 1);
    r.trade(at(9, 30, 20), p2, 200, 2);
    r.trade(at(9, 30, 40), p3, 300, 3);
    r.brokenTrade(at(9, 30, 50), 2);
    // Path 1 -> 3: a single return.
    CHECK_NEAR(r.state.rv.rvSum, sq(lnPrice(p3) - lnPrice(p1)), 1e-15);
}

TEST_CASE(rv_live_sum_after_breaking_tail_then_trading_again) {
    const uint32_t p6 = 1005000, p7 = 1006000, p8 = 1007000;
    Replay r;
    r.trade(at(9, 33, 10), p6, 100, 6);
    r.trade(at(9, 33, 20), p7, 100, 7);
    r.brokenTrade(at(9, 33, 30), 7);
    CHECK_NEAR(r.state.rv.rvSum, 0.0, 1e-15); // only trade 6 remains
    r.trade(at(9, 33, 40), p8, 100, 8);
    // Path 6 -> 8: the live sum must equal (ln p8 - ln p6)^2, matching the ledger.
    CHECK_NEAR(r.state.rv.rvSum, sq(lnPrice(p8) - lnPrice(p6)), 1e-15);
}

// ===========================================================================
// Windowed queries through the prefix histories (minute buckets)
// ===========================================================================

TEST_CASE(vwap_window_includes_every_minute_in_the_window) {
    Replay r;
    r.trade(at(10, 0, 10), 1000000, 100, 1);
    r.trade(at(10, 1, 10), 1002000, 100, 2);

    // [10:00, 10:02): both trades
    std::optional<VWAPIntervalQueryResult> two = r.vwap(at(10, 2, 0), 2);
    CHECK(two.has_value());
    CHECK_EQ(two->deltaNumShares, uint64_t{200});
    CHECK_EQ(two->deltaNotional, __uint128_t{100000000} + __uint128_t{100200000});

    // [10:01, 10:02): the second trade only
    std::optional<VWAPIntervalQueryResult> one = r.vwap(at(10, 2, 0), 1);
    CHECK(one.has_value());
    CHECK_EQ(one->deltaNumShares, uint64_t{100});
    CHECK_EQ(one->deltaNotional, __uint128_t{100200000});
}

TEST_CASE(vwap_window_with_no_trades_is_empty) {
    Replay r;
    r.trade(at(10, 0, 10), 1000000, 100, 1);
    r.vwap(at(10, 1, 0), 1);
    r.vwap(at(10, 2, 0), 1);
    // [10:02, 10:03) has no trades
    CHECK(!r.vwap(at(10, 3, 0), 1).has_value());
}

TEST_CASE(twap_window_integrates_step_function) {
    Replay r;
    r.trade(at(10, 0, 10), 1000000, 100, 1);
    r.trade(at(10, 0, 40), 1002000, 100, 2);
    r.trade(at(10, 2, 30), 1001000, 100, 3);

    // [10:01, 10:02): 100.20 for the whole minute
    std::optional<TWAPIntervalQueryResult> mid = r.twap(at(10, 2, 0), 1);
    CHECK(mid.has_value());
    CHECK_EQ(mid->deltaTotalTime, 60 * NS_PER_SEC);
    CHECK_EQ(mid->deltaPriceTime, __uint128_t{1002000} * (60 * NS_PER_SEC));

    // [10:02, 10:03): 100.20 for 30 s, then 100.10 for 30 s
    std::optional<TWAPIntervalQueryResult> last = r.twap(at(10, 3, 0), 1);
    CHECK(last.has_value());
    CHECK_EQ(last->deltaTotalTime, 60 * NS_PER_SEC);
    CHECK_EQ(last->deltaPriceTime, (__uint128_t{1002000} + __uint128_t{1001000}) * (30 * NS_PER_SEC));

    // [10:00, 10:03) starts at the first print, 10:00:10
    std::optional<TWAPIntervalQueryResult> all = r.twap(at(10, 3, 0), 3);
    CHECK(all.has_value());
    CHECK_EQ(all->deltaTotalTime, 170 * NS_PER_SEC);
    CHECK_EQ(all->deltaPriceTime,
             __uint128_t{1000000} * (30 * NS_PER_SEC) + __uint128_t{1002000} * (110 * NS_PER_SEC)
                 + __uint128_t{1001000} * (30 * NS_PER_SEC));
    CHECK_NEAR(twapAsDouble(*all), 100.15, 1e-9); // 1.7025e17 / 1.7e11 / 1e4 = 100.1470... -> 100.15
}

TEST_CASE(twap_same_price_prints_do_not_change_the_result) {
    Replay withRepeat, without;
    withRepeat.trade(at(10, 0, 10), 1000000, 100, 1);
    withRepeat.trade(at(10, 0, 30), 1000000, 100, 2); // same price
    withRepeat.trade(at(10, 0, 50), 1003000, 100, 3);
    without.trade(at(10, 0, 10), 1000000, 100, 1);
    without.trade(at(10, 0, 50), 1003000, 100, 3);
    std::optional<TWAPIntervalQueryResult> a = withRepeat.twap(at(10, 1, 0), 1);
    std::optional<TWAPIntervalQueryResult> b = without.twap(at(10, 1, 0), 1);
    CHECK(a.has_value() && b.has_value());
    CHECK_EQ(a->deltaTotalTime, b->deltaTotalTime);
    CHECK_EQ(a->deltaPriceTime, b->deltaPriceTime);
}

TEST_CASE(rv_window_sums_returns_of_trades_inside_the_window) {
    const uint32_t p1 = 1000000, p2 = 1010000, p3 = 1005000, p4 = 1005000;
    Replay r;
    r.trade(at(10, 0, 10), p1, 100, 1);
    r.trade(at(10, 0, 40), p2, 100, 2);
    std::optional<RVIntervalQueryResult> m0 = r.rv(at(10, 1, 0), 1);
    CHECK(m0.has_value());
    CHECK_NEAR(m0->deltaRVSum, sq(lnPrice(p2) - lnPrice(p1)), 1e-15);

    r.trade(at(10, 1, 30), p3, 100, 3);
    std::optional<RVIntervalQueryResult> m1 = r.rv(at(10, 2, 0), 1);
    CHECK(m1.has_value());
    CHECK_NEAR(m1->deltaRVSum, sq(lnPrice(p3) - lnPrice(p2)), 1e-15);

    r.trade(at(10, 2, 20), p4, 100, 4); // unchanged price: zero return
    std::optional<RVIntervalQueryResult> m2 = r.rv(at(10, 3, 0), 1);
    CHECK(m2.has_value());
    CHECK_NEAR(m2->deltaRVSum, 0.0, 1e-15);

    std::optional<RVIntervalQueryResult> all = r.rv(at(10, 3, 0), 3);
    CHECK(all.has_value());
    CHECK_NEAR(all->deltaRVSum, sq(lnPrice(p2) - lnPrice(p1)) + sq(lnPrice(p3) - lnPrice(p2)), 1e-15);
}

TEST_CASE(rv_first_trade_of_a_minute_belongs_to_that_minute) {
    // No query lands on 10:01, so the 10:01 snapshot is created by the first
    // trade after the boundary. That trade's return must count in [10:01, 10:02).
    const uint32_t p1 = 1000000, p2 = 1010000;
    Replay r;
    r.trade(at(10, 0, 10), p1, 100, 1);
    r.trade(at(10, 1, 10), p2, 100, 2);
    std::optional<RVIntervalQueryResult> last = r.rv(at(10, 2, 0), 1);
    CHECK(last.has_value());
    CHECK_NEAR(last->deltaRVSum, sq(lnPrice(p2) - lnPrice(p1)), 1e-15);
}

// ===========================================================================
// Oracle replay
// ===========================================================================

struct OracleEvent {
    bool isBreak;
    uint64_t ts;
    uint32_t price;
    uint32_t shares;
    uint64_t match;
    bool printable;
};

struct OracleQuery {
    std::string metric;
    uint64_t now;
    uint64_t minutes;
};

struct OracleExpected {
    std::string metric;
    uint64_t now;
    uint64_t minutes;
    bool none;
    std::string a;
    std::string b;
};

std::vector<std::vector<std::string>> readCsv(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) throw std::runtime_error("cannot open " + path);
    std::vector<std::vector<std::string>> rows;
    std::string line;
    bool headerSkipped = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t firstNonSpace = line.find_first_not_of(" \t");
        if (firstNonSpace == std::string::npos || line[firstNonSpace] == '#') continue;
        if (!headerSkipped) { headerSkipped = true; continue; }
        std::vector<std::string> fields;
        std::stringstream ss(line);
        std::string field;
        while (std::getline(ss, field, ',')) fields.push_back(field);
        while (fields.size() < 6) fields.emplace_back(); // trailing empty fields
        rows.push_back(fields);
    }
    return rows;
}

uint64_t parseU64(const std::string& s) { return std::stoull(s); }

__uint128_t parseU128(const std::string& s) {
    if (s.empty()) throw std::runtime_error("empty 128-bit value");
    __uint128_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') throw std::runtime_error("bad 128-bit value: " + s);
        v = v * 10 + static_cast<unsigned>(c - '0');
    }
    return v;
}

std::vector<OracleEvent> loadEvents(const std::string& path) {
    std::vector<OracleEvent> events;
    for (const auto& f : readCsv(path)) {
        OracleEvent e{};
        e.ts = parseU64(f[1]);
        e.match = parseU64(f[4]);
        if (f[0] == "B") {
            e.isBreak = true;
        } else {
            e.price = static_cast<uint32_t>(std::stoul(f[2]));
            e.shares = static_cast<uint32_t>(std::stoul(f[3]));
            if (f[0] == "E" || f[0] == "P" || f[0] == "Q") e.printable = true;
            else if (f[0] == "C") e.printable = (f[5] == "Y");
            else throw std::runtime_error("unknown event type " + f[0]);
        }
        if (!events.empty() && e.ts < events.back().ts) throw std::runtime_error("events not sorted");
        events.push_back(e);
    }
    return events;
}

std::vector<OracleQuery> loadQueries(const std::string& path) {
    std::vector<OracleQuery> queries;
    for (const auto& f : readCsv(path)) {
        OracleQuery q{f[0], parseU64(f[1]), parseU64(f[2])};
        if (!queries.empty() && q.now < queries.back().now) throw std::runtime_error("queries not sorted");
        queries.push_back(q);
    }
    return queries;
}

std::vector<OracleExpected> loadExpected(const std::string& path) {
    std::vector<OracleExpected> expected;
    for (const auto& f : readCsv(path)) {
        expected.push_back(OracleExpected{f[0], parseU64(f[1]), parseU64(f[2]), f[3] == "NONE", f[4], f[5]});
    }
    return expected;
}

struct EngineAnswer {
    bool none = true;
    std::string a; // shares / total_time / rv sum
    std::string b; // notional / price_time
};

EngineAnswer answer(Replay& r, const OracleQuery& q) {
    EngineAnswer out;
    if (q.metric == "VWAP") {
        if (auto v = r.vwap(q.now, q.minutes)) {
            out.none = false;
            out.a = std::to_string(v->deltaNumShares);
            out.b = uint128_t_to_string(v->deltaNotional);
        }
    } else if (q.metric == "TWAP") {
        if (auto v = r.twap(q.now, q.minutes)) {
            out.none = false;
            out.a = std::to_string(v->deltaTotalTime);
            out.b = uint128_t_to_string(v->deltaPriceTime);
        }
    } else if (q.metric == "RV") {
        if (auto v = r.rv(q.now, q.minutes)) {
            out.none = false;
            out.a = testToString(v->deltaRVSum);
        }
    } else {
        throw std::runtime_error("unknown metric " + q.metric);
    }
    return out;
}

bool matches(const OracleQuery& q, const EngineAnswer& got, const OracleExpected& want) {
    if (got.none || want.none) return got.none == want.none;
    if (q.metric == "RV") {
        double g = std::strtod(got.a.c_str(), nullptr);
        double w = std::strtod(want.a.c_str(), nullptr);
        return std::fabs(g - w) <= 1e-12 + 1e-9 * std::fabs(w);
    }
    return parseU64(got.a) == parseU64(want.a) && parseU128(got.b) == parseU128(want.b);
}

std::string describe(const EngineAnswer& x) {
    if (x.none) return "NONE";
    return x.b.empty() ? x.a : "(" + x.a + ", " + x.b + ")";
}

std::string describe(const OracleExpected& x) {
    if (x.none) return "NONE";
    return x.b.empty() ? x.a : "(" + x.a + ", " + x.b + ")";
}

int runOracleScenario(const std::string& eventsPath, const std::string& queriesPath, const std::string& expectedPath) {
    std::vector<OracleEvent> events = loadEvents(eventsPath);
    std::vector<OracleQuery> queries = loadQueries(queriesPath);
    std::vector<OracleExpected> expected = loadExpected(expectedPath);
    if (expected.size() != queries.size()) throw std::runtime_error("expected.csv does not match queries.csv");

    Replay replay;
    int failed = 0;
    size_t qi = 0;

    auto runQuery = [&](size_t i) {
        const OracleQuery& q = queries[i];
        const OracleExpected& want = expected[i];
        if (want.metric != q.metric || want.now != q.now || want.minutes != q.minutes)
            throw std::runtime_error("expected.csv row " + std::to_string(i) + " does not match queries.csv");
        EngineAnswer got = answer(replay, q);
        bool ok = matches(q, got, want);
        if (!ok) ++failed;
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << q.metric << " now=" << toTimeString(q.now, ':')
                  << " last " << q.minutes << " min: engine=" << describe(got);
        if (!ok) std::cout << " expected=" << describe(want);
        std::cout << "\n";
    };

    // A query at `now` runs after every event with ts < now and before any event with ts >= now.
    for (const OracleEvent& e : events) {
        while (qi < queries.size() && queries[qi].now <= e.ts) runQuery(qi++);
        if (e.isBreak) replay.brokenTrade(e.ts, e.match);
        else replay.trade(e.ts, e.price, e.shares, e.match, e.printable);
    }
    while (qi < queries.size()) runQuery(qi++);

    std::cout << queries.size() - failed << " passed, " << failed << " failed, " << queries.size() << " queries\n";
    return failed;
}

} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "unit") {
            return runAllTests() == 0 ? 0 : 1;
        }
        if (argc == 5 && std::string(argv[1]) == "oracle") {
            return runOracleScenario(argv[2], argv[3], argv[4]) == 0 ? 0 : 1;
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    }
    std::cerr << "usage: metrics_tests unit\n"
                 "       metrics_tests oracle <events.csv> <queries.csv> <expected.csv>\n";
    return 2;
}
