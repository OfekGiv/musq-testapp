/* Per-packet forwarding helpers shared by the responder loop and by the
 * software distributor's master and worker loops (dist.cpp): sending on a
 * worker's own TX queue, the header swap and the mock work. Header-only so
 * every loop inlines them. */
#pragma once

#include "app.hpp"
#include "work.hpp"

#include <rte_ethdev.h>
#include <rte_prefetch.h>

#include <cstring>

#ifdef __SSSE3__
#include <tmmintrin.h>
#endif

/* Packets ahead whose header line is prefetched before it is rewritten: the NIC
 * wrote it to the LLC (DDIO), a few hundred cycles of lookahead hides the load. */
constexpr uint16_t HDR_PREFETCH = 8;

/* The --work sampler, set up by responder_init(). */
extern WorkSampler g_sampler;

/* rte_eth_tx_burst on the worker's own queue; anything refused is freed (l3fwd
 * send_burst / korba flush_tx_batch). Never more than tx_burst packets per call:
 * under mu_sq one call is one eMPW slot. */
static inline uint16_t tx_send(WorkerCtx *w, uint16_t queue, struct rte_mbuf **m, uint16_t n)
{
    const uint16_t sent = rte_eth_tx_burst(w->port_id, queue, m, n);
    w->stats.tx_pkts += sent;
    if (unlikely(sent < n)) {
        w->stats.tx_drops += static_cast<uint64_t>(n - sent);
        for (uint16_t i = sent; i < n; i++)
            rte_pktmbuf_free(m[i]);
    }
    return sent;
}

static inline void flush_txbuf(WorkerCtx *w, WorkerCtx::TxBuf &b)
{
    if (b.len == 0)
        return;
    tx_send(w, b.queue_id, b.m, b.len);
    b.len = 0;
}

/* --tx-lanes > 1: the same, and keep this lane's own share of the counters. */
static inline void flush_lane(WorkerCtx *w, WorkerCtx::TxBuf &b)
{
    if (b.len == 0)
        return;
    const uint16_t sent = tx_send(w, b.queue_id, b.m, b.len);
    b.tx_pkts += sent;
    b.tx_drops += static_cast<uint64_t>(b.len - sent);
    b.len = 0;
}

/* Swap the headers so the packet goes back to its sender. Swapping IPv4
 * addresses and UDP ports keeps both checksums valid (one's-complement sums are
 * order independent), so nothing is recomputed and no TX offload is needed. */
static __rte_always_inline bool swap_headers_data(uint8_t *data, SwapMode mode)
{
    auto *eth = reinterpret_cast<struct rte_ether_hdr *>(data);
    if (mode != SwapMode::None) {
#ifdef __SSSE3__
        /* dst and src MAC (bytes 0-5, 6-11) swapped in one 16-byte shuffle;
         * bytes 12-15 (ether type, start of the IP header) are written back
         * unchanged. Every frame is at least 60 bytes. */
        const __m128i mac_swap = _mm_setr_epi8(6, 7, 8, 9, 10, 11, 0, 1, 2, 3, 4, 5, 12, 13, 14, 15);
        auto *v = reinterpret_cast<__m128i *>(eth);
        _mm_storeu_si128(v, _mm_shuffle_epi8(_mm_loadu_si128(v), mac_swap));
#else
        struct rte_ether_addr tmp;
        rte_ether_addr_copy(&eth->src_addr, &tmp);
        rte_ether_addr_copy(&eth->dst_addr, &eth->src_addr);
        rte_ether_addr_copy(&tmp, &eth->dst_addr);
#endif
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

static __rte_always_inline bool swap_headers(struct rte_mbuf *m, SwapMode mode)
{
    return swap_headers_data(rte_pktmbuf_mtod(m, uint8_t *), mode);
}

/* The responder's per-packet processing of one received burst: header
 * prefetch, mock work (--work) and header swap (--swap), counters into
 * w->stats. work_on, header_work and swap are loop invariants of the caller. */
static __rte_always_inline void process_burst(WorkerCtx *w, struct rte_mbuf **pkts, uint16_t nb_rx,
                                              bool work_on, bool header_work, SwapMode swap)
{
    WorkerStats &s = w->stats;
    /* The PMD has just written each mbuf's first line, so only the packet
     * headers are prefetched. With --swap none and no work the packets go
     * back unread, as testpmd's io mode sends them. */
    if (work_on || swap != SwapMode::None) {
        const uint16_t ahead = RTE_MIN(HDR_PREFETCH, nb_rx);
        for (uint16_t i = 0; i < ahead; i++)
            rte_prefetch0(rte_pktmbuf_mtod(pkts[i], void *));
    }
    if (!work_on && swap != SwapMode::None) {
        uint64_t non_ipv4 = 0;
        for (uint16_t i = 0; i < nb_rx; i++) {
            if (i + HDR_PREFETCH < nb_rx)
                rte_prefetch0(rte_pktmbuf_mtod(pkts[i + HDR_PREFETCH], void *));
            non_ipv4 += !swap_headers(pkts[i], swap);
        }
        s.non_ipv4 += non_ipv4;
    }
    for (uint16_t i = 0; work_on && i < nb_rx; i++) {
        if (i + HDR_PREFETCH < nb_rx)
            rte_prefetch0(rte_pktmbuf_mtod(pkts[i + HDR_PREFETCH], void *));
        struct rte_mbuf *m = pkts[i];

        request_header *rh = nullptr;
        struct rte_udp_hdr *rh_udp = nullptr;
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
        if (!swap_headers(m, swap))
            s.non_ipv4++;
        if (rh) {
            rh->magic_number = RESPONSE_MAGIC;
            /* Payload changed: a UDP checksum would be wrong now, zero = none. */
            if (rh_udp)
                rh_udp->dgram_cksum = 0;
        }
    }
}
