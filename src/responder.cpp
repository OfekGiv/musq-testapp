#include "responder.hpp"
#include "work.hpp"

#include <rte_ethdev.h>
#include <rte_prefetch.h>

static WorkSampler g_sampler;

void responder_init(const AppConfig &cfg, uint64_t tsc_hz)
{
    g_sampler.init(cfg.work, tsc_hz);
}

/* rte_eth_tx_burst on the worker's own queue; anything refused is freed (l3fwd
 * send_burst / korba flush_tx_batch). Never more than tx_burst packets per call:
 * under mu_sq one call is one eMPW slot. */
static inline void tx_send(WorkerCtx *w, struct rte_mbuf **m, uint16_t n)
{
    const uint16_t sent = rte_eth_tx_burst(w->port_id, w->queue_id, m, n);
    w->stats.tx_pkts += sent;
    if (unlikely(sent < n)) {
        w->stats.tx_drops += static_cast<uint64_t>(n - sent);
        for (uint16_t i = sent; i < n; i++)
            rte_pktmbuf_free(m[i]);
    }
}

static inline void flush_txbuf(WorkerCtx *w)
{
    if (w->txbuf.len == 0)
        return;
    tx_send(w, w->txbuf.m, w->txbuf.len);
    w->txbuf.len = 0;
}

/* Swap the headers so the packet goes back to its sender. Swapping IPv4
 * addresses and UDP ports keeps both checksums valid (one's-complement sums are
 * order independent), so nothing is recomputed and no TX offload is needed. */
static inline bool swap_headers(struct rte_mbuf *m, SwapMode mode)
{
    auto *eth = rte_pktmbuf_mtod(m, struct rte_ether_hdr *);
    if (mode != SwapMode::None) {
        struct rte_ether_addr tmp;
        rte_ether_addr_copy(&eth->src_addr, &tmp);
        rte_ether_addr_copy(&eth->dst_addr, &eth->src_addr);
        rte_ether_addr_copy(&tmp, &eth->dst_addr);
    }
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return false;
    if (mode != SwapMode::L2L3)
        return true;
    auto *ip = reinterpret_cast<struct rte_ipv4_hdr *>(eth + 1);
    const rte_be32_t tip = ip->src_addr;
    ip->src_addr = ip->dst_addr;
    ip->dst_addr = tip;
    if (ip->next_proto_id == IPPROTO_UDP) {
        auto *udp = reinterpret_cast<struct rte_udp_hdr *>(
            reinterpret_cast<uint8_t *>(ip) + ((ip->version_ihl & 0x0f) << 2));
        const rte_be16_t tp = udp->src_port;
        udp->src_port = udp->dst_port;
        udp->dst_port = tp;
    }
    return true;
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
    const uint16_t port = w->port_id, queue = w->queue_id;
    const uint16_t rx_burst = cfg.rx_burst, tx_burst = cfg.tx_burst;
    const bool accumulate = cfg.tx_mode == TxMode::Accumulate;
    const bool header_work = g_sampler.kind == WorkKind::Header;
    const bool work_on = g_sampler.enabled();
    const SwapMode swap = cfg.swap;
    const uint64_t drain_tsc = (st->tsc_hz + 999999ULL) / 1000000ULL * cfg.tx_drain_us;
    struct rte_mbuf *pkts[MAX_BURST];

    /* Keep this exact wording: the TRex harness waits for it. */
    APP_LOG_INFO("entering main loop on lcore %u (worker %u, port %u, queue %u)",
                 lcore_id, w->worker_idx, port, queue);

    uint64_t prev_drain = rte_rdtsc();
    s.loop_start_tsc = prev_drain;

    while (!st->force_quit) {
        const uint64_t t0 = rte_rdtsc();

        if (accumulate && unlikely(t0 - prev_drain > drain_tsc)) {
            flush_txbuf(w);
            prev_drain = t0;
        }

        const uint16_t nb_rx = rte_eth_rx_burst(port, queue, pkts, rx_burst);
        if (nb_rx == 0) {
            s.cycles_wasted += rte_rdtsc() - t0;
            continue;
        }
        s.rx_bursts++;
        s.rx_pkts += nb_rx;
        if (nb_rx == rx_burst)
            s.rx_full_bursts++;

        for (uint16_t i = 0; i < nb_rx; i++) {
            if (i + 1 < nb_rx)
                rte_prefetch0(pkts[i + 1]);
            if (i + 2 < nb_rx)
                rte_prefetch0(rte_pktmbuf_mtod(pkts[i + 2], void *));
            struct rte_mbuf *m = pkts[i];

            request_header *rh = nullptr;
            struct rte_udp_hdr *rh_udp = nullptr;
            if (work_on) {
                uint64_t cyc;
                if (header_work) {
                    rh = find_request_header(m, &rh_udp);
                    cyc = rh ? static_cast<uint64_t>(rh->processing_time_us) * g_sampler.cycles_per_us : 0;
                } else {
                    cyc = sample_cycles(g_sampler, w->prng, m);
                }
                if (cyc) {
                    busy_wait_cycles(cyc);
                    s.work_cycles += cyc;
                }
            }
            if (!swap_headers(m, swap))
                s.non_ipv4++;
            if (rh) {
                rh->magic_number = RESPONSE_MAGIC;
                /* Payload changed: a UDP checksum would be wrong now, zero = none. */
                if (rh_udp)
                    rh_udp->dgram_cksum = 0;
            }
        }

        if (!accumulate) {
            /* nb_rx <= rx_burst <= tx_burst: one call, at most one group. */
            tx_send(w, pkts, nb_rx);
        } else {
            for (uint16_t i = 0; i < nb_rx; i++) {
                w->txbuf.m[w->txbuf.len++] = pkts[i];
                if (w->txbuf.len == tx_burst)
                    flush_txbuf(w);
            }
        }
        s.cycles_used += rte_rdtsc() - t0;
    }

    /* Do not transmit after the stop request: just release what is buffered. */
    for (uint16_t i = 0; i < w->txbuf.len; i++)
        rte_pktmbuf_free(w->txbuf.m[i]);
    w->txbuf.len = 0;
    APP_LOG_INFO("lcore %u leaving main loop: rx=%lu tx=%lu drops=%lu", lcore_id,
                 static_cast<unsigned long>(s.rx_pkts), static_cast<unsigned long>(s.tx_pkts),
                 static_cast<unsigned long>(s.tx_drops));
    return 0;
}
