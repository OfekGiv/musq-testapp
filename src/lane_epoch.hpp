/* Per-epoch lane status log.
 *
 * Every lane (TX queue) samples its own work-queue position once per epoch on a
 * common TSC grid: an epoch boundary is a multiple of epoch_tsc on the absolute
 * TSC, so all lanes see the same boundaries without sharing any memory. The
 * sample is taken by the lane's own lcore through
 * rte_pmd_mlx5_txq_lane_status(), which reads only state that lcore writes
 * (wqe_ci, wqe_pi, free, slot counters), and is stored in a log only that lcore
 * writes. The logs are read once, after the workers stopped, and dumped as CSV.
 *
 * The lane's completion-queue state comes from rte_pmd_mlx5_txq_cq_status()
 * in the same way: its own cq_ci and cq_pi, the free entries of its share of
 * the CQ, and how many of its outstanding completion requests the NIC has
 * already completed (found by reading a few of the lane's own CQEs; nothing is
 * consumed). A PMD without that accessor leaves the CQ fields at zero. The
 * doorbell count comes from rte_pmd_mlx5_txq_burst_stats(), likewise.
 *
 * Each record also carries the lane's cumulative RX/TX packet counters and its
 * busy/idle cycle counters (the app's own WorkerStats, written by the same
 * lcore), so the difference between two records is the packets that lane
 * received and the CPU time it spent busy in that epoch. Those need no PMD
 * support: without the accessor the WQ fields stay zero.
 *
 * With --tx-lanes > 1 one lcore posts to several TX queues: it keeps one log
 * per TX queue, sampled together. rx_pkts and the cycle counters are then the
 * lcore's own (the same in each of its lanes' records), tx_pkts and tx_drops
 * the lane's.
 */
#pragma once

#include "app.hpp"

/* One record, 96 bytes, written sequentially by the lane's own lcore. */
struct LaneEpochRec {
    uint64_t tsc;        /* when the sample was taken (first loop pass after the boundary) */
    uint64_t slots;      /* mu_sq slots closed, cumulative */
    uint64_t slot_pkts;  /* packets in those slots, cumulative */
    uint64_t slots_full; /* slots closed with 32 packets, cumulative */
    uint64_t rx_pkts;    /* app counters of this lane, cumulative */
    uint64_t tx_pkts;
    uint64_t tx_drops;
    /* CPU accounting of this lane's loop, cumulative TSC cycles (the same
     * counters as the "CPU busy %" row): loop passes that handled packets,
     * including mock work, and empty polls. */
    uint64_t cycles_busy;
    uint64_t cycles_idle;
    uint64_t doorbells;  /* doorbell register writes by the PMD, cumulative */
    uint16_t wqe_ci;
    uint16_t wqe_pi;
    uint16_t wqe_free;
    uint16_t elts_free;
    uint16_t cq_ci;   /* oldest completion request not consumed (CQ index) */
    uint16_t cq_pi;   /* next completion request to make (CQ index) */
    uint16_t cq_free; /* free entries of the lane's share of the CQ */
    uint16_t cq_done; /* requests the NIC completed that the lane has not consumed */
};

struct LaneEpochLog {
    LaneEpochRec *recs = nullptr;
    uint32_t cap = 0;
    uint32_t n = 0;
    uint64_t missed = 0;    /* samples not stored because the log was full */
    uint64_t next_tsc = 0;  /* next epoch boundary on the common grid */
    uint64_t epoch_tsc = 0;
};

/* Look up the optional accessors of the prom-dpdk mlx5 PMD. Call once, before
 * the workers start. */
void lane_pmd_init();
/* Doorbell register writes the PMD has made for a TX queue, cumulative. Reads
 * a record the queue's own lcore writes, kept apart from its hot fields, so it
 * may be called from any thread. False when the PMD cannot tell. */
bool lane_doorbells(uint16_t port, uint16_t queue, uint64_t *doorbells);
/* Resolve the PMD accessor and allocate one log per worker. Returns 0 when the
 * feature is off or ready, -1 on error. */
int lane_epoch_init(AppState &st);
/* Check that the loaded mlx5 PMD's mu_sq slot size matches --pkts-per-wqe.
 * Returns 0 when it matches or cannot be checked (no accessor), -1 on mismatch. */
int lane_slot_check(const AppState &st);
/* Write all logs to cfg.lane_epoch_out (call after the workers have stopped). */
void lane_epoch_dump(AppState &st);
void lane_epoch_cleanup(AppState &st);
/* Slow path: take one sample of every TX lane of the worker and advance to the
 * next boundary. */
void lane_epoch_sample(WorkerCtx *w, uint64_t now);

/* Call once per loop iteration with the TSC value the loop already read. */
static inline void lane_epoch_tick(WorkerCtx *w, uint64_t now)
{
    LaneEpochLog *log = w->epoch_log[0];
    if (log != nullptr && unlikely(now >= log->next_tsc))
        lane_epoch_sample(w, now);
}
