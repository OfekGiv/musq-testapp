#include "lane_epoch.hpp"

#include <rte_cycles.h>
#include <rte_malloc.h>

#include <dlfcn.h>

#include <cinttypes>
#include <cstdio>

/* Mirror of struct rte_pmd_mlx5_txq_lane_status (prom-dpdk rte_pmd_mlx5.h).
 * The accessor checks the size we pass, so a layout change is caught at init. */
struct PmdLaneStatus {
    uint16_t wqe_ci;
    uint16_t wqe_pi;
    uint16_t wqe_free;
    uint16_t wqe_s;
    uint16_t elts_free;
    uint16_t log_group_size;
    uint16_t slot_wqebbs;
    uint16_t slot_max_pkts;
    uint64_t slots;
    uint64_t slot_pkts;
    uint64_t slots_full;
};
using lane_status_fn = int (*)(uint16_t, uint16_t, PmdLaneStatus *, size_t);
static lane_status_fn g_lane_status = nullptr;
static unsigned g_slot_wqebbs = 0; /* 0 = unknown (no accessor) */
static unsigned g_wqe_s = 0;       /* TX ring size in WQEBBs, 0 = unknown */

/* Mirror of struct rte_pmd_mlx5_txq_cq_status (prom-dpdk rte_pmd_mlx5.h). */
struct PmdCqStatus {
    uint16_t cq_ci;
    uint16_t cq_pi;
    uint16_t cq_step;
    uint16_t cqe_s;
    uint16_t cq_used;
    uint16_t cq_free;
    uint16_t cq_done;
    uint16_t reserved;
};
using cq_status_fn = int (*)(uint16_t, uint16_t, PmdCqStatus *, size_t);
static cq_status_fn g_cq_status = nullptr;
static unsigned g_cqe_s = 0;   /* TX CQ size in entries (shared by a mu_sq group), 0 = unknown */
static unsigned g_cq_step = 0; /* CQ entries between two of a lane's completion requests */

/* Mirror of struct rte_pmd_mlx5_txq_burst_stats (prom-dpdk rte_pmd_mlx5.h). */
struct PmdBurstStats {
    uint64_t calls;
    uint64_t passes;
    uint64_t doorbells;
};
using burst_stats_fn = int (*)(uint16_t, uint16_t, PmdBurstStats *, size_t);
static burst_stats_fn g_burst_stats = nullptr;

void lane_pmd_init()
{
    g_burst_stats = reinterpret_cast<burst_stats_fn>(dlsym(RTLD_DEFAULT, "rte_pmd_mlx5_txq_burst_stats"));
}

bool lane_doorbells(uint16_t port, uint16_t queue, uint64_t *doorbells)
{
    PmdBurstStats bs = {};
    if (g_burst_stats == nullptr || g_burst_stats(port, queue, &bs, sizeof(bs)) != 0)
        return false;
    *doorbells = bs.doorbells;
    return true;
}

static lane_status_fn resolve_lane_status()
{
    /* Looked up at run time: only the prom-dpdk mlx5 PMD has it, and only a
     * build that links that PMD directly (the DOCA build) can see the symbol. */
    return reinterpret_cast<lane_status_fn>(dlsym(RTLD_DEFAULT, "rte_pmd_mlx5_txq_lane_status"));
}

int lane_slot_check(const AppState &st)
{
    if (st.cfg.musq_group_eff() == 0)
        return 0;
    const lane_status_fn fn = resolve_lane_status();
    PmdLaneStatus ls = {};
    if (fn == nullptr || fn(st.port_id, 0, &ls, sizeof(ls)) != 0) {
        APP_LOG_WARN("cannot read the PMD's mu_sq slot size in this build; --pkts-per-wqe %u is not verified",
                     st.cfg.pkts_per_wqe);
        return 0;
    }
    APP_LOG_INFO("mu_sq slot: %u packet(s), %u WQEBB(s), lane step %u WQEBBs", ls.slot_max_pkts, ls.slot_wqebbs,
                 static_cast<unsigned>(ls.slot_wqebbs) << ls.log_group_size);
    if (ls.slot_max_pkts != st.cfg.pkts_per_wqe) {
        APP_LOG_ERR("--pkts-per-wqe %u, but this mlx5 PMD was built for %u packets per mu_sq slot; "
                    "start through scripts/run.sh, which picks the matching binary",
                    st.cfg.pkts_per_wqe, ls.slot_max_pkts);
        return -1;
    }
    return 0;
}

int lane_epoch_init(AppState &st)
{
    const AppConfig &cfg = st.cfg;
    if (cfg.lane_epoch_us == 0)
        return 0;
    g_lane_status = resolve_lane_status();
    PmdLaneStatus probe = {};
    if (g_lane_status == nullptr) {
        /* The per-lane RX/TX packet counters are the app's own and need no PMD
         * support: keep recording those, with the WQ columns left at zero. */
        APP_LOG_WARN("--lane-epoch-us: no rte_pmd_mlx5_txq_lane_status in this build/PMD (needs the prom-dpdk "
                     "mlx5 PMD linked into the app, i.e. the DOCA build); recording RX/TX packet counters only");
    } else {
        const int rc = g_lane_status(st.port_id, 0, &probe, sizeof(probe));
        if (rc != 0) {
            APP_LOG_ERR("rte_pmd_mlx5_txq_lane_status(port %u, queue 0) failed: %d", st.port_id, rc);
            return -1;
        }
        g_slot_wqebbs = probe.slot_wqebbs;
        g_wqe_s = probe.wqe_s;
        /* Added to the PMD later than the lane status: optional on its own. */
        g_cq_status = reinterpret_cast<cq_status_fn>(dlsym(RTLD_DEFAULT, "rte_pmd_mlx5_txq_cq_status"));
        PmdCqStatus cq = {};
        if (g_cq_status != nullptr && g_cq_status(st.port_id, 0, &cq, sizeof(cq)) != 0)
            g_cq_status = nullptr;
        if (g_cq_status == nullptr) {
            APP_LOG_WARN("--lane-epoch-us: no usable rte_pmd_mlx5_txq_cq_status in this mlx5 PMD; "
                         "the CQ columns stay zero");
        } else {
            g_cqe_s = cq.cqe_s;
            g_cq_step = cq.cq_step;
        }
    }
    const uint64_t epoch_tsc = st.tsc_hz / 1000000ULL * cfg.lane_epoch_us;
    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        for (unsigned l = 0; l < w.nb_tx_lanes; l++) {
            auto *log = static_cast<LaneEpochLog *>(rte_zmalloc("lane_epoch_log", sizeof(LaneEpochLog), RTE_CACHE_LINE_SIZE));
            if (log == nullptr)
                return -1;
            log->recs = static_cast<LaneEpochRec *>(rte_zmalloc_socket(
                "lane_epoch_recs", sizeof(LaneEpochRec) * cfg.lane_epoch_max, RTE_CACHE_LINE_SIZE,
                static_cast<int>(rte_lcore_to_socket_id(w.lcore_id))));
            if (log->recs == nullptr) {
                APP_LOG_ERR("cannot allocate %u lane-epoch records for lcore %u", cfg.lane_epoch_max, w.lcore_id);
                return -1;
            }
            log->cap = cfg.lane_epoch_max;
            log->epoch_tsc = epoch_tsc;
            log->next_tsc = 0; /* first loop pass samples immediately and aligns to the grid */
            w.epoch_log[l] = log;
        }
    }
    APP_LOG_INFO("lane epoch log: %lu us epochs (%lu TSC ticks, mu_sq group %u), %u records per lane, output %s",
                 static_cast<unsigned long>(cfg.lane_epoch_us), static_cast<unsigned long>(epoch_tsc),
                 probe.log_group_size ? 1u << probe.log_group_size : 0u, cfg.lane_epoch_max,
                 cfg.lane_epoch_out.c_str());
    return 0;
}

void lane_epoch_sample(WorkerCtx *w, uint64_t now)
{
    const bool one_lane = w->nb_tx_lanes == 1;
    for (unsigned l = 0; l < w->nb_tx_lanes; l++) {
        LaneEpochLog *log = w->epoch_log[l];
        /* Next boundary on the absolute TSC grid, identical on every lane. */
        log->next_tsc = (now / log->epoch_tsc + 1) * log->epoch_tsc;
        if (unlikely(log->n >= log->cap)) {
            log->missed++;
            continue;
        }
        const WorkerCtx::TxBuf &b = w->txbuf[l];
        PmdLaneStatus st = {};
        if (g_lane_status != nullptr && unlikely(g_lane_status(w->port_id, b.queue_id, &st, sizeof(st)) != 0))
            continue;
        LaneEpochRec &r = log->recs[log->n++];
        r.tsc = now;
        r.slots = st.slots;
        r.slot_pkts = st.slot_pkts;
        r.slots_full = st.slots_full;
        r.rx_pkts = w->stats.rx_pkts;
        r.tx_pkts = one_lane ? w->stats.tx_pkts : b.tx_pkts;
        r.tx_drops = one_lane ? w->stats.tx_drops : b.tx_drops;
        r.cycles_busy = w->stats.cycles_used;
        r.cycles_idle = w->stats.cycles_wasted;
        r.wqe_ci = st.wqe_ci;
        r.wqe_pi = st.wqe_pi;
        r.wqe_free = st.wqe_free;
        r.elts_free = st.elts_free;
        PmdCqStatus cq = {};
        if (g_cq_status != nullptr)
            g_cq_status(w->port_id, b.queue_id, &cq, sizeof(cq));
        r.cq_ci = cq.cq_ci;
        r.cq_pi = cq.cq_pi;
        r.cq_free = cq.cq_free;
        r.cq_done = cq.cq_done;
        uint64_t db = 0;
        lane_doorbells(w->port_id, b.queue_id, &db);
        r.doorbells = db;
    }
}

void lane_epoch_dump(AppState &st)
{
    const AppConfig &cfg = st.cfg;
    if (cfg.lane_epoch_us == 0 || cfg.lane_epoch_out.empty())
        return;
    uint64_t t0 = UINT64_MAX, epoch_tsc = 0, total = 0;
    unsigned nb_lanes = 0;
    for (unsigned i = 0; i < st.nb_workers; i++) {
        for (unsigned l = 0; l < st.workers[i].nb_tx_lanes; l++) {
            const LaneEpochLog *log = st.workers[i].epoch_log[l];
            nb_lanes++;
            if (log == nullptr || log->n == 0)
                continue;
            epoch_tsc = log->epoch_tsc;
            t0 = RTE_MIN(t0, log->recs[0].tsc);
            total += log->n;
        }
    }
    if (total == 0) {
        APP_LOG_WARN("lane epoch log is empty, nothing written");
        return;
    }
    FILE *f = fopen(cfg.lane_epoch_out.c_str(), "w");
    if (f == nullptr) {
        APP_LOG_ERR("cannot open %s for writing", cfg.lane_epoch_out.c_str());
        return;
    }
    fprintf(f, "# epoch_us=%" PRIu64 " tsc_hz=%" PRIu64 " lanes=%u wq=%d slot_wqebbs=%u slot_pkts=%u wqe_s=%u"
               " cq=%d cqe_s=%u cq_step=%u db=%d db_wqes=%u\n",
            cfg.lane_epoch_us, st.tsc_hz, nb_lanes, g_lane_status != nullptr ? 1 : 0,
            g_slot_wqebbs ? g_slot_wqebbs : 9u, cfg.pkts_per_wqe, g_wqe_s,
            g_cq_status != nullptr ? 1 : 0, g_cqe_s, g_cq_step,
            g_burst_stats != nullptr ? 1 : 0, cfg.db_wqes);
    fprintf(f, "lane,lcore,epoch,t_us,wqe_ci,wqe_pi,wqe_free,elts_free,slots,slot_pkts,slots_full,rx_pkts,tx_pkts,tx_drops,"
               "cycles_busy,cycles_idle,busy_pct,cq_ci,cq_pi,cq_free,cq_done,doorbells\n");
    const uint64_t e0 = t0 / epoch_tsc;
    for (unsigned i = 0; i < st.nb_workers * st.cfg.tx_lanes; i++) {
        /* One log per TX queue; the "lane" column is the TX queue index. */
        const WorkerCtx &w = st.workers[i / st.cfg.tx_lanes];
        const unsigned lane = w.txbuf[i % st.cfg.tx_lanes].queue_id;
        const LaneEpochLog *log = w.epoch_log[i % st.cfg.tx_lanes];
        if (log == nullptr)
            continue;
        uint64_t prev_busy = 0, prev_idle = 0; /* the first record covers loop start .. sample */
        for (uint32_t k = 0; k < log->n; k++) {
            const LaneEpochRec &r = log->recs[k];
            /* CPU busy % of this lane since its previous record, i.e. in this epoch. */
            const uint64_t db = r.cycles_busy - prev_busy, di = r.cycles_idle - prev_idle;
            const double busy_pct = db + di ? 100.0 * static_cast<double>(db) / static_cast<double>(db + di) : 0.0;
            prev_busy = r.cycles_busy;
            prev_idle = r.cycles_idle;
            fprintf(f, "%u,%u,%" PRIu64 ",%.1f,%u,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64
                       ",%" PRIu64 ",%" PRIu64 ",%.1f,%u,%u,%u,%u,%" PRIu64 "\n",
                    lane, w.lcore_id, r.tsc / epoch_tsc - e0,
                    static_cast<double>(r.tsc - t0) * 1e6 / static_cast<double>(st.tsc_hz),
                    r.wqe_ci, r.wqe_pi, r.wqe_free, r.elts_free, r.slots, r.slot_pkts, r.slots_full,
                    r.rx_pkts, r.tx_pkts, r.tx_drops, r.cycles_busy, r.cycles_idle, busy_pct,
                    r.cq_ci, r.cq_pi, r.cq_free, r.cq_done, r.doorbells);
        }
        if (log->missed)
            APP_LOG_WARN("lcore %u: lane epoch log full, %lu later samples not stored (raise --lane-epoch-max)",
                         w.lcore_id, static_cast<unsigned long>(log->missed));
    }
    fclose(f);
    APP_LOG_INFO("lane epoch log: %lu records written to %s", static_cast<unsigned long>(total), cfg.lane_epoch_out.c_str());
}

void lane_epoch_cleanup(AppState &st)
{
    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        for (unsigned l = 0; l < MAX_TX_LANES; l++) {
            if (w.epoch_log[l]) {
                rte_free(w.epoch_log[l]->recs);
                rte_free(w.epoch_log[l]);
                w.epoch_log[l] = nullptr;
            }
        }
    }
}
