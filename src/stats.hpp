#pragma once

#include "app.hpp"

#include <rte_ethdev.h>
#include <rte_lcore.h>

#include <cstdio>
#include <string>
#include <vector>

/* Per-core counter snapshot (relaxed loads of WorkerStats). */
struct CoreSnapshot {
    uint64_t rx_pkts = 0, tx_pkts = 0, tx_drops = 0, rx_bursts = 0, rx_full_bursts = 0,
             non_ipv4 = 0, work_cycles = 0, cycles_used = 0, cycles_wasted = 0,
             req_sent = 0, resp_matched = 0, resp_unmatched = 0, req_lost = 0, tx_batch_capped = 0;
};

class StatsReporter {
public:
    int init(AppState &st);
    /* Take fresh baselines (end of warmup): totals restart from zero. */
    void rebase();
    /* Print (and optionally append CSV for) one window of window_tsc cycles. */
    void report(uint64_t window_tsc, bool is_final);
    void close();

private:
    void report_summary(const struct rte_eth_stats &s, uint64_t window_tsc, double window_s);
    void report_responder_summary(const struct rte_eth_stats &s, uint64_t window_tsc, double window_s);
    /* Reads the summary xstats (see sum_ids_) into vals; false if unavailable. */
    bool read_summary_xstats(std::vector<uint64_t> &vals) const;
    void report_port(const struct rte_eth_stats &s, uint64_t window_tsc, double window_s, bool is_final);
    void report_xstats(uint64_t window_tsc, double window_s, bool is_final);
    void report_cores(uint64_t window_tsc, double window_s, bool is_final);
    void report_rtt(bool is_final);
    void csv_row(const char *scope, const std::string &id, const char *metric,
                 uint64_t total, uint64_t delta, double rate);
    CoreSnapshot snapshot(const WorkerCtx &w) const;

    AppState *st_ = nullptr;
    uint64_t hz_ = 0;
    uint64_t start_tsc_ = 0;
    uint64_t window_index_ = 0;
    /* current window identity for CSV rows */
    double elapsed_s_ = 0, window_s_ = 0;
    bool is_final_ = false;

    struct rte_eth_stats port_base_{}, port_prev_{};
    std::vector<std::string> xs_names_;
    std::vector<uint64_t> xs_ids_, xs_base_, xs_prev_;
    std::vector<CoreSnapshot> core_base_, core_prev_;
    /* Responder summary: NIC xstats by id, each -1 when the PMD lacks it.
     * [0] rx_phy_packets (offered at the port), [1] rx_prio0_buf_discard_packets
     * (dropped in the NIC before any RX queue). */
    int64_t sum_ids_[2] = {-1, -1};
    uint64_t sum_base_[2] = {0, 0}, sum_prev_[2] = {0, 0};
    FILE *csv_ = nullptr;
};

/* DPDK /eal/lcore/usage telemetry callback (rte_lcore_register_usage_cb). */
int lcore_usage_cb(unsigned int lcore_id, struct rte_lcore_usage *usage);
