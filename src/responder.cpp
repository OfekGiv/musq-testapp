#include "responder.hpp"
#include "forward.hpp"
#include "lane_epoch.hpp"

#include <rte_ethdev.h>

#include <cstring>

WorkSampler g_sampler;

void responder_init(const AppConfig &cfg, uint64_t tsc_hz)
{
    g_sampler.init(cfg.work, tsc_hz);
}

int responder_loop(void *arg)
{
    AppState *st = static_cast<AppState *>(arg);
    const unsigned lcore_id = rte_lcore_id();
    const int widx = st->lcore_to_worker[lcore_id];
    if (widx < 0) {
        APP_LOG_INFO("lcore %u has no worker slot, exiting", lcore_id);
        return 0;
    }
    WorkerCtx *w = &st->workers[widx];
    WorkerStats &s = w->stats;
    const AppConfig &cfg = st->cfg;
    const uint16_t port = w->port_id;
    /* RX queues rxq0 .. rxq_end - 1, one burst from each in turn (--rx-queues);
     * TX goes to this worker's own queue(s) whichever RX queue it came from. */
    const uint16_t rxq0 = w->queue_id, rxq_end = static_cast<uint16_t>(rxq0 + w->nb_rx_queues);
    const bool multi_rxq = w->nb_rx_queues > 1;
    const uint16_t txq = w->txbuf[0].queue_id;
    uint16_t rxq = rxq0;
    const uint16_t rx_burst = cfg.rx_burst, tx_burst = cfg.tx_burst;
    const uint16_t nb_lanes = w->nb_tx_lanes;
    const bool accumulate = cfg.tx_mode == TxMode::Accumulate;
    const bool db_flush = cfg.db_wqes != 0;
    const bool header_work = g_sampler.kind == WorkKind::Header;
    const bool work_on = g_sampler.enabled();
    const SwapMode swap = cfg.swap;
    const uint64_t drain_tsc = (st->tsc_hz + 999999ULL) / 1000000ULL * cfg.tx_drain_us;
    struct rte_mbuf *pkts[MAX_BURST];

    /* Keep this exact wording: the TRex harness waits for it. */
    APP_LOG_INFO("entering main loop on lcore %u (worker %u, port %u, queue %u)",
                 lcore_id, w->worker_idx, port, rxq0);

    uint64_t prev_drain = rte_rdtsc();
    s.loop_start_tsc = prev_drain;
    uint64_t t_prev = prev_drain;
    bool prev_busy = false;

    while (!st->force_quit) {
        /* One TSC read per pass: the cycles since the previous read belong to
         * the previous pass, busy if it handled packets, wasted if it polled
         * nothing. */
        const uint64_t t0 = rte_rdtsc();
        if (prev_busy)
            s.cycles_used += t0 - t_prev;
        else
            s.cycles_wasted += t0 - t_prev;
        t_prev = t0;

        lane_epoch_tick(w, t0);

        if ((accumulate || db_flush) && unlikely(t0 - prev_drain > drain_tsc)) {
            if (!accumulate) {
                /* nothing buffered in immediate mode */
            } else if (likely(nb_lanes == 1)) {
                flush_txbuf(w, w->txbuf[0]);
            } else {
                for (uint16_t l = 0; l < nb_lanes; l++)
                    flush_lane(w, w->txbuf[l]);
            }
            if (unlikely(db_flush)) {
                /* --db-wqes: the PMD rings a lane's doorbell only every so
                 * many WQEs, so the last ones of a burst of traffic wait. A
                 * lane that posted nothing since the previous tick gets a
                 * burst of no packets, which makes the PMD ring for them. */
                for (uint16_t l = 0; l < nb_lanes; l++) {
                    WorkerCtx::TxBuf &b = w->txbuf[l];
                    const uint64_t sent = nb_lanes == 1 ? s.tx_pkts : b.tx_pkts;
                    if (sent == b.db_seen)
                        rte_eth_tx_burst(port, b.queue_id, pkts, 0);
                    b.db_seen = sent;
                }
            }
            prev_drain = t0;
        }

        const uint16_t nb_rx = rte_eth_rx_burst(port, rxq, pkts, rx_burst);
        if (unlikely(multi_rxq))
            rxq = static_cast<uint16_t>(rxq + 1 == rxq_end ? rxq0 : rxq + 1);
        prev_busy = nb_rx != 0;
        if (nb_rx == 0)
            continue;
        s.rx_bursts++;
        s.rx_pkts += nb_rx;
        if (nb_rx == rx_burst)
            s.rx_full_bursts++;

        process_burst(w, pkts, nb_rx, work_on, header_work, swap);

        if (unlikely(nb_lanes != 1)) {
            /* One lcore, several TX queues (mu_sq lanes): deal the packets to
             * the lanes in turn, packet i of the stream to lane i % nb_lanes,
             * as round-robin steering to one core per lane would. Each lane
             * has its own buffer and gets its own rte_eth_tx_burst calls. */
            uint16_t lane = w->next_lane;
            for (uint16_t i = 0; i < nb_rx; i++) {
                WorkerCtx::TxBuf &b = w->txbuf[lane];
                b.m[b.len++] = pkts[i];
                if (accumulate && b.len == tx_burst)
                    flush_lane(w, b);
                lane = static_cast<uint16_t>((lane + 1) & (nb_lanes - 1));
            }
            w->next_lane = lane;
            if (!accumulate)
                for (uint16_t l = 0; l < nb_lanes; l++)
                    flush_lane(w, w->txbuf[l]);
        } else if (!accumulate) {
            /* nb_rx <= rx_burst <= tx_burst: one call, at most one group. */
            tx_send(w, txq, pkts, nb_rx);
        } else {
            WorkerCtx::TxBuf &b = w->txbuf[0];
            uint16_t len = b.len;
            if (len == 0 && nb_rx == tx_burst) {
                /* A whole TX burst with nothing buffered: send it as it is. */
                tx_send(w, txq, pkts, nb_rx);
            } else {
                for (uint16_t i = 0; i < nb_rx;) {
                    const uint16_t n = RTE_MIN(static_cast<uint16_t>(nb_rx - i), static_cast<uint16_t>(tx_burst - len));
                    memcpy(&b.m[len], &pkts[i], n * sizeof(pkts[0]));
                    len = static_cast<uint16_t>(len + n);
                    i = static_cast<uint16_t>(i + n);
                    if (len == tx_burst) {
                        tx_send(w, txq, b.m, len);
                        len = 0;
                    }
                }
                b.len = len;
            }
        }
    }

    /* Do not transmit after the stop request: just release what is buffered. */
    for (uint16_t l = 0; l < nb_lanes; l++) {
        for (uint16_t i = 0; i < w->txbuf[l].len; i++)
            rte_pktmbuf_free(w->txbuf[l].m[i]);
        w->txbuf[l].len = 0;
    }
    APP_LOG_INFO("lcore %u leaving main loop: rx=%lu tx=%lu drops=%lu", lcore_id,
                 static_cast<unsigned long>(s.rx_pkts), static_cast<unsigned long>(s.tx_pkts),
                 static_cast<unsigned long>(s.tx_drops));
    return 0;
}
