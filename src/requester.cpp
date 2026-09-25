#include "requester.hpp"
#include "config.hpp"
#include "work.hpp"

#include <hdr/hdr_histogram.h>

#include <rte_arp.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_udp.h>

#include <atomic>
#include <cmath>
#include <cstring>

/* Request id layout: [owner worker idx : 16][sequence : 48]. The slot is the
 * low bits of the sequence. */
static constexpr unsigned ID_SEQ_BITS = 48;
static constexpr uint64_t ID_SEQ_MASK = (1ULL << ID_SEQ_BITS) - 1;
static inline uint64_t id_owner(uint64_t id) { return id >> ID_SEQ_BITS; }

struct alignas(32) RequestSlot {
    std::atomic<uint64_t> id{0};      /* request id currently owning the slot (0 = never used) */
    uint64_t send_tsc = 0;            /* written by the owner before id (release) */
    std::atomic<uint64_t> done_id{0}; /* id of the last completed request in this slot */
};

struct RequestTracker {
    RequestSlot *slots = nullptr;
    uint32_t mask = 0;
    uint64_t next_seq = 1;            /* sequence 0 is never used (id 0 = empty slot) */
    uint64_t owner_bits = 0;
    /* pacing (korba requester_calculate_batch_size) */
    uint64_t last_send_tsc = 0;
    uint64_t pacing_acc = 0;
    /* packet template */
    uint8_t tmpl[2048];
    uint16_t tmpl_len = 0;
    uint16_t hdr_off = 0;             /* offset of request_header in the frame */
    uint16_t l4_off = 0;              /* offset of UDP header / ESP SPI */
    uint16_t next_sport = 0, next_dport = 0;
    uint32_t next_sn = 0;
};

/* Service-time sampler in microseconds (same distributions as --work). */
struct ServiceSampler {
    WorkKind kind = WorkKind::Fixed;
    uint32_t lo_us = 0, hi_us = 0;
    uint32_t hi_threshold = 0;
    uint32_t exp_table[WorkSampler::EXP_TABLE_SIZE] = {};

    void init(const WorkSpec &spec)
    {
        const auto ns_to_us = [](uint64_t ns) { return static_cast<uint32_t>((ns + 500) / 1000); };
        kind = spec.kind == WorkKind::Header ? WorkKind::Fixed : spec.kind;
        lo_us = ns_to_us(spec.lo_ns);
        hi_us = ns_to_us(spec.hi_ns);
        hi_threshold = static_cast<uint32_t>((static_cast<uint64_t>(spec.pct_hi) * 0xFFFFFFFFULL) / 100ULL);
        if (kind == WorkKind::Exp) {
            const double mean = static_cast<double>(spec.lo_ns);
            for (unsigned i = 0; i < WorkSampler::EXP_TABLE_SIZE; i++) {
                const double u = (static_cast<double>(i) + 0.5) / static_cast<double>(WorkSampler::EXP_TABLE_SIZE);
                exp_table[i] = ns_to_us(static_cast<uint64_t>(-mean * std::log(u)));
            }
        }
    }
    inline uint32_t sample(uint64_t &prng) const
    {
        switch (kind) {
        case WorkKind::Bimodal:
            return static_cast<uint32_t>(xorshift64s(prng) >> 32) < hi_threshold ? hi_us : lo_us;
        case WorkKind::Exp:
            return exp_table[xorshift64s(prng) >> (64 - WorkSampler::EXP_TABLE_BITS)];
        default:
            return lo_us;
        }
    }
};

static ServiceSampler g_service;
static uint64_t g_hz;

static bool parse_mac(const std::string &s, struct rte_ether_addr *out)
{
    return rte_ether_unformat_addr(s.c_str(), out) == 0;
}

static bool parse_ipv4(const std::string &s, rte_be32_t *out)
{
    unsigned a, b, c, d;
    char tail;
    if (sscanf(s.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return false;
    *out = rte_cpu_to_be_32((a << 24) | (b << 16) | (c << 8) | d);
    return true;
}

static int build_template(const AppState &st, WorkerCtx &w, RequestTracker &t)
{
    const AppConfig &cfg = st.cfg;
    struct rte_ether_addr src_mac, dst_mac;
    rte_be32_t src_ip, dst_ip;
    if (cfg.src_mac.empty())
        rte_eth_macaddr_get(w.port_id, &src_mac);
    else if (!parse_mac(cfg.src_mac, &src_mac)) {
        APP_LOG_ERR("bad --src-mac '%s'", cfg.src_mac.c_str());
        return -1;
    }
    if (!parse_mac(cfg.dst_mac, &dst_mac)) {
        APP_LOG_ERR("bad --dst-mac '%s'", cfg.dst_mac.c_str());
        return -1;
    }
    if (!parse_ipv4(cfg.src_ip, &src_ip) || !parse_ipv4(cfg.dst_ip, &dst_ip)) {
        APP_LOG_ERR("bad --src-ip/--dst-ip");
        return -1;
    }
    const uint16_t content = static_cast<uint16_t>(cfg.frame_size - 4);
    memset(t.tmpl, 0, sizeof(t.tmpl));
    auto *eth = reinterpret_cast<struct rte_ether_hdr *>(t.tmpl);
    rte_ether_addr_copy(&src_mac, &eth->src_addr);
    rte_ether_addr_copy(&dst_mac, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    auto *ip = reinterpret_cast<struct rte_ipv4_hdr *>(eth + 1);
    ip->version_ihl = 0x45;
    ip->time_to_live = 64;
    ip->src_addr = src_ip;
    ip->dst_addr = dst_ip;
    ip->total_length = rte_cpu_to_be_16(static_cast<uint16_t>(content - sizeof(*eth)));
    t.l4_off = sizeof(*eth) + sizeof(*ip);
    if (cfg.traffic == TrafficKind::Udp) {
        ip->next_proto_id = IPPROTO_UDP;
        auto *udp = reinterpret_cast<struct rte_udp_hdr *>(ip + 1);
        udp->src_port = rte_cpu_to_be_16(cfg.sport_lo);
        udp->dst_port = rte_cpu_to_be_16(cfg.dport_lo);
        udp->dgram_len = rte_cpu_to_be_16(static_cast<uint16_t>(content - t.l4_off));
        udp->dgram_cksum = 0; /* no checksum (allowed on IPv4); ports change per packet */
        t.hdr_off = t.l4_off + sizeof(*udp);
    } else {
        ip->next_proto_id = 50; /* ESP: SPI(4) + SN(4), then our header as the "payload" */
        auto *spi = reinterpret_cast<uint32_t *>(t.tmpl + t.l4_off);
        spi[0] = rte_cpu_to_be_32(0x00001000);
        spi[1] = 0;
        t.hdr_off = t.l4_off + 8;
    }
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);
    auto *rh = reinterpret_cast<request_header *>(t.tmpl + t.hdr_off);
    rh->magic_number = REQUEST_MAGIC;
    t.tmpl_len = content;
    t.next_sport = cfg.sport_lo;
    t.next_dport = cfg.dport_lo;
    t.next_sn = 0;
    return 0;
}

int requester_init(AppState &st)
{
    g_hz = st.tsc_hz;
    g_service.init(st.cfg.service_time);
    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        auto *t = static_cast<RequestTracker *>(rte_zmalloc("req_tracker", sizeof(RequestTracker), RTE_CACHE_LINE_SIZE));
        if (t == nullptr) {
            APP_LOG_ERR("cannot allocate tracker for lcore %u", w.lcore_id);
            return -1;
        }
        new (t) RequestTracker();
        t->slots = static_cast<RequestSlot *>(rte_zmalloc_socket("req_slots", sizeof(RequestSlot) * st.cfg.tracker_size,
                                                                 RTE_CACHE_LINE_SIZE, rte_lcore_to_socket_id(w.lcore_id)));
        if (t->slots == nullptr) {
            APP_LOG_ERR("cannot allocate %u tracker slots for lcore %u", st.cfg.tracker_size, w.lcore_id);
            return -1;
        }
        for (uint32_t k = 0; k < st.cfg.tracker_size; k++)
            new (&t->slots[k]) RequestSlot();
        t->mask = st.cfg.tracker_size - 1;
        t->owner_bits = static_cast<uint64_t>(i) << ID_SEQ_BITS;
        if (build_template(st, w, *t) < 0)
            return -1;
        if (hdr_init(1, 10000000, 3, &w.rtt_hist) != 0) {
            APP_LOG_ERR("hdr_init failed for lcore %u", w.lcore_id);
            return -1;
        }
        w.tracker = t;
        APP_LOG_INFO("lcore %u requester: %s frames of %u B, %lu rps, %u slots, service %s",
                     w.lcore_id, st.cfg.traffic == TrafficKind::Udp ? "UDP" : "ESP", st.cfg.frame_size,
                     static_cast<unsigned long>(st.cfg.rps), st.cfg.tracker_size,
                     work_spec_to_string(st.cfg.service_time).c_str());
    }
    return 0;
}

void requester_cleanup(AppState &st)
{
    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        if (w.tracker) {
            rte_free(w.tracker->slots);
            rte_free(w.tracker);
            w.tracker = nullptr;
        }
        if (w.rtt_hist) {
            hdr_close(w.rtt_hist);
            w.rtt_hist = nullptr;
        }
    }
}

/* korba requester_calculate_batch_size: exact integer pacing on the TSC. */
static inline uint32_t pacing_batch(RequestTracker &t, uint64_t rps, uint64_t now)
{
    if (unlikely(t.last_send_tsc == 0)) {
        t.last_send_tsc = now;
        return 0;
    }
    const uint64_t elapsed = now - t.last_send_tsc;
    t.last_send_tsc = now;
    const unsigned __int128 acc = static_cast<unsigned __int128>(t.pacing_acc) +
                                  static_cast<unsigned __int128>(elapsed) * rps;
    const uint64_t batch = static_cast<uint64_t>(acc / g_hz);
    t.pacing_acc = static_cast<uint64_t>(acc % g_hz);
    return batch > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(batch);
}

/* Locate the request header in a received frame for the configured traffic kind. */
static inline const request_header *response_header(const struct rte_mbuf *m, TrafficKind kind)
{
    const auto *eth = rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return nullptr;
    const auto *ip = reinterpret_cast<const struct rte_ipv4_hdr *>(eth + 1);
    const unsigned ihl = (ip->version_ihl & 0x0f) << 2;
    unsigned off = sizeof(*eth) + ihl;
    if (kind == TrafficKind::Udp) {
        if (ip->next_proto_id != IPPROTO_UDP)
            return nullptr;
        off += sizeof(struct rte_udp_hdr);
    } else {
        if (ip->next_proto_id != 50)
            return nullptr;
        off += 8;
    }
    if (m->data_len < off + sizeof(request_header))
        return nullptr;
    const auto *rh = reinterpret_cast<const request_header *>(rte_pktmbuf_mtod_offset(m, const uint8_t *, off));
    return rh->magic_number == RESPONSE_MAGIC ? rh : nullptr;
}

static inline void complete(AppState *st, WorkerCtx *rx_w, uint64_t id, uint64_t now)
{
    const uint64_t owner = id_owner(id);
    if (owner >= st->nb_workers) {
        rx_w->stats.resp_unmatched++;
        return;
    }
    RequestTracker *t = st->workers[owner].tracker;
    RequestSlot &slot = t->slots[id & t->mask];
    if (slot.id.load(std::memory_order_acquire) != id ||
        slot.done_id.load(std::memory_order_relaxed) == id) {
        rx_w->stats.resp_unmatched++;
        return;
    }
    const uint64_t rtt = now - slot.send_tsc;
    slot.done_id.store(id, std::memory_order_relaxed);
    rx_w->stats.resp_matched++;
    if (st->measuring.load(std::memory_order_relaxed))
        hdr_record_value(rx_w->rtt_hist, static_cast<int64_t>((rtt * 1000000ULL) / g_hz));
}

int requester_loop(void *arg)
{
    AppState *st = static_cast<AppState *>(arg);
    const unsigned lcore_id = rte_lcore_id();
    const int widx = st->lcore_to_worker[lcore_id];
    if (widx < 0)
        return 0;
    WorkerCtx *w = &st->workers[widx];
    WorkerStats &s = w->stats;
    RequestTracker &t = *w->tracker;
    const AppConfig &cfg = st->cfg;
    const uint16_t port = w->port_id, queue = w->queue_id;
    const uint16_t rx_burst = cfg.rx_burst;
    const TrafficKind kind = cfg.traffic;
    const uint64_t rps = cfg.rps;
    struct rte_mbuf *pkts[MAX_BURST];

    APP_LOG_INFO("entering main loop on lcore %u (requester %u, port %u, queue %u)",
                 lcore_id, w->worker_idx, port, queue);
    s.loop_start_tsc = rte_rdtsc();

    while (!st->force_quit) {
        const uint64_t t0 = rte_rdtsc();
        unsigned handled = 0;

        /* Responses (any lcore's) arriving on this queue. */
        const uint16_t nb_rx = rte_eth_rx_burst(port, queue, pkts, rx_burst);
        if (nb_rx) {
            const uint64_t now = rte_rdtsc();
            s.rx_bursts++;
            s.rx_pkts += nb_rx;
            if (nb_rx == rx_burst)
                s.rx_full_bursts++;
            for (uint16_t i = 0; i < nb_rx; i++) {
                if (i + 1 < nb_rx)
                    rte_prefetch0(rte_pktmbuf_mtod(pkts[i + 1], void *));
                const request_header *rh = response_header(pkts[i], kind);
                if (rh)
                    complete(st, w, rh->request_id, now);
                else
                    s.non_ipv4++;
            }
            rte_pktmbuf_free_bulk(pkts, nb_rx);
            handled += nb_rx;
        }

        /* Paced request batch. */
        uint32_t batch = pacing_batch(t, rps, t0);
        if (batch > MAX_BURST) {
            s.tx_batch_capped++;
            batch = MAX_BURST;
        }
        if (batch) {
            if (unlikely(rte_pktmbuf_alloc_bulk(w->pool, pkts, batch) != 0)) {
                s.tx_drops += batch;
            } else {
                const uint64_t send_tsc = rte_rdtsc();
                for (uint32_t i = 0; i < batch; i++) {
                    struct rte_mbuf *m = pkts[i];
                    uint8_t *d = rte_pktmbuf_mtod(m, uint8_t *);
                    rte_memcpy(d, t.tmpl, t.tmpl_len);
                    m->data_len = t.tmpl_len;
                    m->pkt_len = t.tmpl_len;

                    const uint64_t id = t.owner_bits | (t.next_seq++ & ID_SEQ_MASK);
                    RequestSlot &slot = t.slots[id & t.mask];
                    const uint64_t prev = slot.id.load(std::memory_order_relaxed);
                    if (prev != 0 && slot.done_id.load(std::memory_order_relaxed) != prev)
                        s.req_lost++;
                    slot.send_tsc = send_tsc;
                    slot.id.store(id, std::memory_order_release);

                    auto *rh = reinterpret_cast<request_header *>(d + t.hdr_off);
                    rh->request_id = id;
                    rh->processing_time_us = g_service.sample(w->prng);
                    if (kind == TrafficKind::Udp) {
                        auto *udp = reinterpret_cast<struct rte_udp_hdr *>(d + t.l4_off);
                        udp->src_port = rte_cpu_to_be_16(t.next_sport);
                        udp->dst_port = rte_cpu_to_be_16(t.next_dport);
                        t.next_sport = t.next_sport >= cfg.sport_hi ? cfg.sport_lo : t.next_sport + 1;
                        t.next_dport = t.next_dport >= cfg.dport_hi ? cfg.dport_lo : t.next_dport + 1;
                    } else {
                        auto *sn = reinterpret_cast<uint32_t *>(d + t.l4_off + 4);
                        *sn = rte_cpu_to_be_32(t.next_sn++);
                    }
                }
                const uint16_t sent = rte_eth_tx_burst(port, queue, pkts, static_cast<uint16_t>(batch));
                s.tx_pkts += sent;
                s.req_sent += sent;
                if (unlikely(sent < batch)) {
                    /* Never left the host: release their slots so they are not
                     * counted as lost later, and count them as TX drops. */
                    for (uint32_t i = sent; i < batch; i++) {
                        const auto *rh = rte_pktmbuf_mtod_offset(pkts[i], const request_header *, t.hdr_off);
                        RequestSlot &slot = t.slots[rh->request_id & t.mask];
                        slot.done_id.store(rh->request_id, std::memory_order_relaxed);
                        rte_pktmbuf_free(pkts[i]);
                    }
                    s.tx_drops += batch - sent;
                }
                handled += sent;
            }
        }

        const uint64_t dt = rte_rdtsc() - t0;
        if (handled)
            s.cycles_used += dt;
        else
            s.cycles_wasted += dt;
    }
    APP_LOG_INFO("lcore %u leaving main loop: sent=%lu matched=%lu unmatched=%lu lost=%lu", lcore_id,
                 static_cast<unsigned long>(s.req_sent), static_cast<unsigned long>(s.resp_matched),
                 static_cast<unsigned long>(s.resp_unmatched), static_cast<unsigned long>(s.req_lost));
    return 0;
}
