#include "requester.hpp"
#include "esp_hw.hpp"
#include "config.hpp"
#include "work.hpp"

#include <fmt/format.h>
#include <hdr/hdr_histogram.h>

#include <rte_arp.h>
#include <rte_ethdev.h>
#include <rte_ether.h>
#include <rte_flow.h>
#include <rte_ip.h>
#include <rte_malloc.h>
#include <rte_prefetch.h>
#include <rte_udp.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <vector>

/* Request id layout: [owner worker idx : 16][sequence : 48]. The slot is the
 * low bits of the sequence. */
static constexpr unsigned ID_SEQ_BITS = 48;
static constexpr uint64_t ID_SEQ_MASK = (1ULL << ID_SEQ_BITS) - 1;
static inline uint64_t id_owner(uint64_t id) { return id >> ID_SEQ_BITS; }
/* Requests ahead of the current one whose tracker slot is prefetched. */
static constexpr uint64_t SLOT_PREFETCH = 32;
/* Packets ahead of the current one whose mbuf header and data line are prefetched. */
static constexpr uint32_t PKT_PREFETCH = 8;

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
    uint32_t rx_show = 0;             /* --esp-encap hw: responses still to describe in the log */
    uint16_t hdr_off = 0;             /* offset of request_header in the frame */
    uint16_t l4_off = 0;              /* offset of UDP header / ESP SPI */
    uint16_t next_sport = 0, next_dport = 0;
    uint32_t next_sn = 0;
    rte_be32_t src_ip = 0;            /* this lcore's source IP (responses come back to it) */
    /* ESP SN order check (check_esp_sn): this lcore's SPI, the last sequence
     * number seen, and whether the SNs are its own consecutive sequence (gaps
     * then mean loss) or a share of the port-wide one (--esp-sa port). */
    rte_be32_t spi = 0;
    uint32_t last_sn = 0;
    bool sn_seen = false;
    bool sn_own_seq = false;
    bool sn_check = false;
    struct rte_mempool *tx_pool = nullptr; /* prefill: TX-only pool; copy: the worker's RX pool */
    int32_t mbuf_data_off = -1;       /* prefill: data - mbuf, identical for every mbuf (-1 unknown, -2 varies) */
    /* --tx-file / --rx-record */
    uint16_t chunk_off = 0;           /* offset of the 8 B chunk index; the chunk's file bytes follow */
    uint64_t next_chunk = 0;          /* next chunk of the file to send (== its ESP SN) */
    bool file_sent_logged = false;
    uint8_t *rec_data = nullptr;      /* recorded chunk bytes, g_file.chunk per arrival */
    uint64_t *rec_idx = nullptr;      /* recorded chunk index per arrival */
    uint64_t rec_cap = 0;             /* arrivals the buffers hold */
};

/* --tx-file: the file every requester lcore sends once, mapped read-only once
 * for all of them (this is the traffic generator, not the DUT). */
struct FileSource {
    const uint8_t *data = nullptr;
    uint64_t size = 0;
    uint64_t chunks = 0;              /* ceil(size / chunk) */
    uint16_t chunk = 0;               /* file bytes carried per request */
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
static FileSource g_file;
static uint64_t g_hz;
static std::vector<struct rte_flow *> g_steer_flows;

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

/* The frame the lcores build: with --esp-encap hw the NIC adds the ESP
 * encapsulation, so they build the UDP frame that goes inside it. */
static inline TrafficKind frame_kind(const AppConfig &cfg)
{
    return cfg.esp_hw ? TrafficKind::Udp : cfg.traffic;
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
    /* One source IP per lcore: the responder swaps the addresses, so the
     * response's destination IP identifies the owner (flow steering) or at
     * least varies the RSS hash (rss). --src-ips names them explicitly. */
    if (!cfg.src_ips.empty()) {
        if (!parse_ipv4(cfg.src_ips[w.worker_idx], &src_ip)) {
            APP_LOG_ERR("bad --src-ips entry '%s'", cfg.src_ips[w.worker_idx].c_str());
            return -1;
        }
    } else if (cfg.rx_steer != RxSteer::None) {
        src_ip = rte_cpu_to_be_32(rte_be_to_cpu_32(src_ip) + w.worker_idx);
    }
    t.src_ip = src_ip;
    /* --esp-encap hw: a plain UDP frame, sized so that it has the requested
     * size on the wire after the NIC's ESP encapsulation. */
    const uint16_t content = cfg.esp_hw ? esp_hw_lengths(cfg.frame_size).plain_content
                                        : static_cast<uint16_t>(cfg.frame_size - 4);
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
    if (frame_kind(cfg) == TrafficKind::Udp) {
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
        /* The same SPI scheme as the NIC's encapsulation: one per lcore by default. */
        spi[0] = rte_cpu_to_be_32(cfg.esp_spi + (cfg.esp_sa_per_lcore ? w.worker_idx : 0u));
        spi[1] = 0;
        t.spi = spi[0];
        t.hdr_off = t.l4_off + 8;
    }
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);
    auto *rh = reinterpret_cast<request_header *>(t.tmpl + t.hdr_off);
    rh->magic_number = REQUEST_MAGIC;
    /* Constant per run when fixed: the send path then leaves it alone. */
    rh->processing_time_us = g_service.kind == WorkKind::Fixed ? g_service.lo_us : 0;
    t.tmpl_len = content;
    t.chunk_off = static_cast<uint16_t>(t.hdr_off + sizeof(request_header));
    t.next_sport = cfg.sport_lo;
    t.next_dport = cfg.dport_lo;
    t.next_sn = 0;
    return 0;
}

/* Ingress rule: IPv4 dst == this lcore's source IP -> its own RX queue. Rules
 * outrank the port's default RSS flow, so every response is completed on the
 * lcore that owns the request (no cross-lcore tracker traffic) and ESP
 * responses, which RSS would pile onto one queue, spread over all lcores. */
static int install_steer_flow(const WorkerCtx &w, const RequestTracker &t)
{
    struct rte_flow_attr attr;
    memset(&attr, 0, sizeof(attr));
    attr.ingress = 1;
    struct rte_flow_item_ipv4 ip_spec, ip_mask;
    memset(&ip_spec, 0, sizeof(ip_spec));
    memset(&ip_mask, 0, sizeof(ip_mask));
    ip_spec.hdr.dst_addr = t.src_ip;
    ip_mask.hdr.dst_addr = RTE_BE32(0xffffffff);
    struct rte_flow_item pattern[3];
    memset(pattern, 0, sizeof(pattern));
    pattern[0].type = RTE_FLOW_ITEM_TYPE_ETH;
    pattern[1].type = RTE_FLOW_ITEM_TYPE_IPV4;
    pattern[1].spec = &ip_spec;
    pattern[1].mask = &ip_mask;
    pattern[2].type = RTE_FLOW_ITEM_TYPE_END;
    struct rte_flow_action_queue queue;
    queue.index = w.queue_id;
    struct rte_flow_action actions[2];
    memset(actions, 0, sizeof(actions));
    actions[0].type = RTE_FLOW_ACTION_TYPE_QUEUE;
    actions[0].conf = &queue;
    actions[1].type = RTE_FLOW_ACTION_TYPE_END;
    struct rte_flow_error err;
    memset(&err, 0, sizeof(err));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &t.src_ip, ip, sizeof(ip));
    if (rte_flow_validate(w.port_id, &attr, pattern, actions, &err) != 0) {
        APP_LOG_ERR("rte_flow_validate(dst %s -> queue %u): %s (type %d)", ip, w.queue_id,
                    err.message ? err.message : "?", err.type);
        return -1;
    }
    struct rte_flow *f = rte_flow_create(w.port_id, &attr, pattern, actions, &err);
    if (f == nullptr) {
        APP_LOG_ERR("rte_flow_create(dst %s -> queue %u): %s (type %d)", ip, w.queue_id,
                    err.message ? err.message : "?", err.type);
        return -1;
    }
    g_steer_flows.push_back(f);
    APP_LOG_INFO("lcore %u: responses to %s steered to queue %u", w.lcore_id, ip, w.queue_id);
    return 0;
}

/* rte_mempool_obj_iter callback: write the frame template into one mbuf. */
static void prefill_mbuf(struct rte_mempool *mp, void *opaque, void *obj, unsigned idx)
{
    (void)mp;
    (void)idx;
    auto *t = static_cast<RequestTracker *>(opaque);
    auto *m = static_cast<struct rte_mbuf *>(obj);
    rte_memcpy(rte_pktmbuf_mtod(m, void *), t->tmpl, t->tmpl_len);
    /* The send path derives the data address from the mbuf pointer (no header
     * load); that needs the same buf_addr + data_off layout in every mbuf. */
    const auto off = static_cast<int32_t>(rte_pktmbuf_mtod(m, uint8_t *) - reinterpret_cast<uint8_t *>(m));
    if (t->mbuf_data_off == -1)
        t->mbuf_data_off = off;
    else if (t->mbuf_data_off != off)
        t->mbuf_data_off = -2;
    /* rte_pktmbuf_init already left one segment, no next, refcnt 1 and zeroed
     * offload fields; the lengths never change either. mlx5 returns TX mbufs
     * to the pool untouched, so these stay valid across reuse and the send
     * path can take raw objects with rte_mempool_get_bulk (no per-mbuf reset,
     * which would touch the mbuf's second cache line). */
    m->data_len = t->tmpl_len;
    m->pkt_len = t->tmpl_len;
}

/* Dedicated TX pool whose mbufs carry the template from the start. TX never
 * touches the data and this pool is not used for RX, so after the first fill
 * only the per-request fields have to be written: no 500 B copy per packet. */
static int create_tx_pool(AppState &st, WorkerCtx &w, RequestTracker &t)
{
    char name[RTE_MEMPOOL_NAMESIZE];
    snprintf(name, sizeof(name), "tx_pool_lc%u", w.lcore_id);
    /* Just enough for the TX ring, this lcore's mempool cache and the bursts in
     * flight (2^n - 1, the rte_mempool optimum): a small pool keeps the mbuf
     * headers being recycled inside L2 instead of streaming through DRAM. */
    uint64_t want = static_cast<uint64_t>(st.cfg.txd) + 3u * MAX_BURST + 2u * RTE_MEMPOOL_CACHE_MAX_SIZE;
    uint64_t pw = 1;
    while (pw - 1 < want)
        pw <<= 1;
    const unsigned n = static_cast<unsigned>(pw - 1);
    struct rte_mempool *p = rte_pktmbuf_pool_create(name, n, RTE_MEMPOOL_CACHE_MAX_SIZE, 0,
                                                    RTE_MBUF_DEFAULT_BUF_SIZE,
                                                    static_cast<int>(rte_lcore_to_socket_id(w.lcore_id)));
    if (p == nullptr) {
        APP_LOG_ERR("cannot create %s (%u mbufs): %s", name, n, rte_strerror(rte_errno));
        return -1;
    }
    const uint32_t filled = rte_mempool_obj_iter(p, prefill_mbuf, &t);
    if (t.mbuf_data_off < 0)
        APP_LOG_WARN("lcore %u: TX pool mbufs have differing data offsets; using the header path", w.lcore_id);
    t.tx_pool = p;
    APP_LOG_INFO("lcore %u: TX pool %s, %u mbufs prefilled with the %u B template", w.lcore_id, name, filled, t.tmpl_len);
    return 0;
}

static int open_file_source(const AppConfig &cfg, const RequestTracker &t)
{
    const int fd = open(cfg.tx_file.c_str(), O_RDONLY);
    if (fd < 0) {
        APP_LOG_ERR("--tx-file %s: %s", cfg.tx_file.c_str(), strerror(errno));
        return -1;
    }
    struct stat sb;
    if (fstat(fd, &sb) != 0 || sb.st_size <= 0) {
        APP_LOG_ERR("--tx-file %s: empty or unreadable", cfg.tx_file.c_str());
        close(fd);
        return -1;
    }
    void *p = mmap(nullptr, static_cast<size_t>(sb.st_size), PROT_READ, MAP_PRIVATE | MAP_POPULATE, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        APP_LOG_ERR("--tx-file %s: mmap: %s", cfg.tx_file.c_str(), strerror(errno));
        return -1;
    }
    g_file.data = static_cast<const uint8_t *>(p);
    g_file.size = static_cast<uint64_t>(sb.st_size);
    g_file.chunk = static_cast<uint16_t>(t.tmpl_len - t.chunk_off - 8);
    g_file.chunks = (g_file.size + g_file.chunk - 1) / g_file.chunk;
    APP_LOG_INFO("stream test: %s, %lu B = %lu chunks of %u B; every requester lcore sends it once as its own flow",
                 cfg.tx_file.c_str(), static_cast<unsigned long>(g_file.size),
                 static_cast<unsigned long>(g_file.chunks), g_file.chunk);
    return 0;
}

/* Record buffers, faulted in now so the RX path never takes a page fault. Room
 * for every chunk once plus 1/8 for duplicates. */
static int alloc_record(const WorkerCtx &w, RequestTracker &t)
{
    t.rec_cap = g_file.chunks + g_file.chunks / 8 + 4096;
    const size_t data_len = t.rec_cap * g_file.chunk, idx_len = t.rec_cap * sizeof(uint64_t);
    void *d = mmap(nullptr, data_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    void *x = mmap(nullptr, idx_len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (d == MAP_FAILED || x == MAP_FAILED) {
        APP_LOG_ERR("lcore %u: cannot allocate %zu B of record buffers", w.lcore_id, data_len + idx_len);
        return -1;
    }
    t.rec_data = static_cast<uint8_t *>(d);
    t.rec_idx = static_cast<uint64_t *>(x);
    return 0;
}

static bool write_all(const std::string &path, const void *buf, size_t len)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (f == nullptr)
        return false;
    const bool ok = fwrite(buf, 1, len, f) == len;
    return fclose(f) == 0 && ok;
}

/* Write this lcore's recording: <prefix>.w<worker>.data (chunk bytes in arrival
 * order: byte-identical to the file when nothing was lost, duplicated,
 * reordered or corrupted and the file size is a multiple of the chunk size),
 * .idx (uint64 chunk index per arrival) and .json (what the compare needs). */
static void dump_record(const AppConfig &cfg, const WorkerCtx &w, const RequestTracker &t)
{
    const WorkerStats &s = w.stats;
    const std::string base = fmt::format("{}.w{}", cfg.rx_record, w.worker_idx);
    const bool ok = write_all(base + ".data", t.rec_data, s.rec_chunks * g_file.chunk) &&
                    write_all(base + ".idx", t.rec_idx, s.rec_chunks * sizeof(uint64_t));
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &t.src_ip, ip, sizeof(ip));
    const std::string meta = fmt::format(
        "{{\"file\": \"{}\", \"file_size\": {}, \"chunk_bytes\": {}, \"file_chunks\": {}, "
        "\"worker\": {}, \"lcore\": {}, \"src_ip\": \"{}\", \"chunks_sent\": {}, "
        "\"arrivals\": {}, \"bad_frames\": {}, \"overflow\": {}, \"tx_refused\": {}}}\n",
        cfg.tx_file, g_file.size, g_file.chunk, g_file.chunks, w.worker_idx, w.lcore_id, ip, t.next_chunk,
        s.rec_chunks, s.rec_bad, s.rec_overflow, s.tx_drops);
    if (!ok || !write_all(base + ".json", meta.data(), meta.size()))
        APP_LOG_ERR("lcore %u: cannot write %s.*: %s", w.lcore_id, base.c_str(), strerror(errno));
    else
        APP_LOG_INFO("lcore %u: recorded %lu chunks (%lu bad frames, %lu overflow) to %s.{data,idx,json}",
                     w.lcore_id, static_cast<unsigned long>(s.rec_chunks), static_cast<unsigned long>(s.rec_bad),
                     static_cast<unsigned long>(s.rec_overflow), base.c_str());
}

int requester_init(AppState &st)
{
    g_hz = st.tsc_hz;
    g_service.init(st.cfg.service_time);
    std::vector<rte_be32_t> src_ips;
    std::vector<uint32_t> spis(st.nb_workers, 0);
    /* The SN check needs each lcore's responses on its own queue: the flow
     * rule (sw encap) or the per-source-IP DOCA rule (hw encap) gives that. */
    const bool sn_check = st.cfg.esp_sn_check && st.cfg.traffic == TrafficKind::Esp &&
                          (st.cfg.esp_hw ? st.cfg.rx_steer != RxSteer::None || st.nb_workers == 1
                                         : st.cfg.rx_steer == RxSteer::Flow);
    if (st.cfg.esp_sn_check && st.cfg.traffic == TrafficKind::Esp && !sn_check)
        APP_LOG_WARN("ESP SN order check off: it needs each lcore's responses on its own queue (--rx-steer flow)");
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
        if (!st.cfg.tx_file.empty() && g_file.data == nullptr && open_file_source(st.cfg, *t) < 0)
            return -1;
        if (!st.cfg.rx_record.empty() && alloc_record(w, *t) < 0)
            return -1;
        if (st.cfg.tx_fill == TxFill::Prefill) {
            if (create_tx_pool(st, w, *t) < 0)
                return -1;
        } else {
            t->tx_pool = w.pool;
        }
        if (hdr_init(1, 10000000, 3, &w.rtt_hist) != 0) {
            APP_LOG_ERR("hdr_init failed for lcore %u", w.lcore_id);
            return -1;
        }
        w.tracker = t;
        t->sn_check = sn_check;
        /* Gaps are loss only when the lcore owns its SN sequence. */
        t->sn_own_seq = !st.cfg.esp_hw || st.cfg.esp_sa_per_lcore;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &t->src_ip, ip, sizeof(ip));
        if (st.cfg.esp_hw) {
            const EspHwLen len = esp_hw_lengths(st.cfg.frame_size);
            APP_LOG_INFO("lcore %u requester: plain UDP frames of %u B from %s, ESP-encapsulated and encrypted by the NIC "
                         "to %u B on the wire (incl. FCS), %lu rps",
                         w.lcore_id, len.plain_content + 4, ip, len.wire_content + 4,
                         static_cast<unsigned long>(st.cfg.rps));
            t->rx_show = 4;
            src_ips.push_back(t->src_ip);
            continue; /* DOCA Flow steers the responses, see esp_hw_start() */
        }
        APP_LOG_INFO("lcore %u requester: %s frames of %u B from %s, %lu rps, %u slots, service %s",
                     w.lcore_id, st.cfg.traffic == TrafficKind::Udp ? "UDP" : "ESP", st.cfg.frame_size, ip,
                     static_cast<unsigned long>(st.cfg.rps), st.cfg.tracker_size,
                     work_spec_to_string(st.cfg.service_time).c_str());
        if (st.cfg.rx_steer == RxSteer::Flow && install_steer_flow(w, *t) < 0)
            return -1;
    }
    if (st.cfg.esp_hw) {
        if (esp_hw_start(st, src_ips.data(), spis.data()) < 0)
            return -1;
        /* The SPI the NIC writes into this lcore's packets (its SA's). */
        for (unsigned i = 0; i < st.nb_workers; i++)
            st.workers[i].tracker->spi = rte_cpu_to_be_32(spis[i]);
    }
    if (sn_check)
        APP_LOG_INFO("ESP SN order check on: each lcore expects its responses with increasing sequence numbers%s",
                     st.cfg.esp_hw && !st.cfg.esp_sa_per_lcore ? " (one SA for all lcores: gaps are not counted)" : "");
    return 0;
}

void requester_stop(AppState &st)
{
    if (st.cfg.esp_hw)
        esp_hw_stop();
    struct rte_flow_error err;
    for (struct rte_flow *f : g_steer_flows)
        if (rte_flow_destroy(st.port_id, f, &err) != 0)
            APP_LOG_WARN("rte_flow_destroy: %s", err.message ? err.message : "?");
    g_steer_flows.clear();
}

void requester_cleanup(AppState &st)
{
    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        if (w.tracker) {
            if (w.tracker->rec_data)
                munmap(w.tracker->rec_data, w.tracker->rec_cap * g_file.chunk);
            if (w.tracker->rec_idx)
                munmap(w.tracker->rec_idx, w.tracker->rec_cap * sizeof(uint64_t));
            if (w.tracker->tx_pool && w.tracker->tx_pool != w.pool)
                rte_mempool_free(w.tracker->tx_pool);
            rte_free(w.tracker->slots);
            rte_free(w.tracker);
            w.tracker = nullptr;
        }
        if (w.rtt_hist) {
            hdr_close(w.rtt_hist);
            w.rtt_hist = nullptr;
        }
    }
    if (g_file.data) {
        munmap(const_cast<uint8_t *>(g_file.data), g_file.size);
        g_file = FileSource();
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

/* ESP SN order check: the SPI and sequence number of an ESP packet are clear
 * text, also when the NIC encrypted the payload, so a response's SN tells
 * whether the DUT kept this lcore's packets in order. With one SA per lcore
 * the NIC numbers its packets 0, 1, 2, .. in the order they leave, so a
 * response SN not above the previous one is a reorder and a jump is loss.
 * Two compares per packet. */
static inline void check_esp_sn(RequestTracker &t, WorkerStats &s, const struct rte_mbuf *m)
{
    const auto *eth = rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return;
    const auto *ip = reinterpret_cast<const struct rte_ipv4_hdr *>(eth + 1);
    if (ip->next_proto_id != 50)
        return;
    const unsigned off = sizeof(*eth) + ((ip->version_ihl & 0x0f) << 2);
    if (m->data_len < off + 8)
        return;
    const auto *esp = rte_pktmbuf_mtod_offset(m, const rte_be32_t *, off);
    if (esp[0] != t.spi) {
        s.spi_foreign++;
        return;
    }
    const uint32_t sn = rte_be_to_cpu_32(esp[1]);
    if (!t.sn_seen) {
        t.sn_seen = true;
        t.last_sn = sn;
        return;
    }
    const int32_t d = static_cast<int32_t>(sn - t.last_sn); /* wrap-safe */
    if (d <= 0) {
        s.sn_reorder++;
        const uint64_t back = static_cast<uint64_t>(-static_cast<int64_t>(d));
        if (back > s.sn_max_back)
            s.sn_max_back = back;
        return;
    }
    if (d > 1 && t.sn_own_seq)
        s.sn_gap_pkts += static_cast<uint64_t>(d - 1);
    t.last_sn = sn;
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

/* --rx-record: append the chunk a response carries. Only the frame shape is
 * checked (the responder may or may not have flipped the request magic). */
static inline void record_chunk(RequestTracker &t, WorkerStats &s, const struct rte_mbuf *m, TrafficKind kind)
{
    const uint16_t ch = g_file.chunk;
    const auto *eth = rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
    const auto *ip = reinterpret_cast<const struct rte_ipv4_hdr *>(eth + 1);
    if (unlikely(m->data_len < t.chunk_off + 8u + ch || eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4) ||
                 ip->version_ihl != 0x45 || ip->next_proto_id != (kind == TrafficKind::Udp ? IPPROTO_UDP : 50))) {
        s.rec_bad++;
        return;
    }
    if (unlikely(s.rec_chunks == t.rec_cap)) {
        s.rec_overflow++;
        return;
    }
    const uint8_t *cp = rte_pktmbuf_mtod_offset(m, const uint8_t *, t.chunk_off);
    memcpy(&t.rec_idx[s.rec_chunks], cp, sizeof(uint64_t));
    rte_memcpy(t.rec_data + s.rec_chunks * ch, cp + 8, ch);
    s.rec_chunks++;
}

/* Write the per-request fields of one TX batch. Every piece of state lives in
 * a local for the whole batch: stores into the frame go through byte pointers
 * that may alias anything, so fields read through `t`/`cfg` would otherwise be
 * reloaded (and the cursors stored back) after every packet. Fields that are
 * constant for the run (fixed service time, single destination port) are
 * already in the template and are not written here. */
template <TrafficKind KIND, bool COPY>
__attribute__((noinline)) static void fill_requests(RequestTracker &t, const AppConfig &cfg, WorkerCtx *w,
                                 struct rte_mbuf **pkts, uint32_t batch, uint64_t send_tsc)
{
    RequestSlot *const slots = t.slots;
    const uint64_t mask = t.mask;
    const uint64_t owner = t.owner_bits;
    const uint16_t hdr_off = t.hdr_off, l4_off = t.l4_off, len = t.tmpl_len;
    const uint16_t sp_lo = cfg.sport_lo, sp_hi = cfg.sport_hi;
    const uint16_t dp_lo = cfg.dport_lo, dp_hi = cfg.dport_hi;
    const bool vary_sport = sp_lo != sp_hi, vary_dport = dp_lo != dp_hi;
    const bool vary_service = g_service.kind != WorkKind::Fixed;
    const bool fixed_off = !COPY && t.mbuf_data_off >= 0;
    const int32_t data_off = t.mbuf_data_off;
    uint64_t seq = t.next_seq;
    uint16_t sp = t.next_sport, dp = t.next_dport;
    uint32_t sn = t.next_sn;
    uint64_t prng = w->prng;
    uint64_t lost = 0;
    const uint8_t *const fdata = g_file.data;
    const uint64_t fsize = g_file.size, fchunk = g_file.chunk;
    const uint16_t chunk_off = t.chunk_off;
    uint64_t chunk = t.next_chunk;

    for (uint32_t i = 0; i < batch; i++) {
        struct rte_mbuf *m = pkts[i];
        if (i + PKT_PREFETCH < batch) {
            struct rte_mbuf *ahead = pkts[i + PKT_PREFETCH];
            rte_prefetch0(ahead); /* header: read by the PMD in rte_eth_tx_burst */
            if (fixed_off)
                __builtin_prefetch(reinterpret_cast<uint8_t *>(ahead) + data_off + l4_off, 1, 3);
        }
        uint8_t *d = fixed_off ? reinterpret_cast<uint8_t *>(m) + data_off : rte_pktmbuf_mtod(m, uint8_t *);
        if (COPY) {
            rte_memcpy(d, t.tmpl, len);
            m->data_len = len;
            m->pkt_len = len;
        }

        const uint64_t id = owner | (seq++ & ID_SEQ_MASK);
        RequestSlot &slot = slots[id & mask];
        /* The slot array is far larger than the caches and walked
         * sequentially: fetch the line a few requests ahead, for write. */
        __builtin_prefetch(&slots[(id + SLOT_PREFETCH) & mask], 1, 3);
        const uint64_t prev = slot.id.load(std::memory_order_relaxed);
        lost += (prev != 0) & (slot.done_id.load(std::memory_order_relaxed) != prev);
        slot.send_tsc = send_tsc;
        slot.id.store(id, std::memory_order_release);

        auto *rh = reinterpret_cast<request_header *>(d + hdr_off);
        rh->request_id = id;
        if (vary_service)
            rh->processing_time_us = g_service.sample(prng);
        if (KIND == TrafficKind::Udp) {
            auto *udp = reinterpret_cast<struct rte_udp_hdr *>(d + l4_off);
            if (vary_sport) {
                udp->src_port = rte_cpu_to_be_16(sp);
                sp = sp >= sp_hi ? sp_lo : static_cast<uint16_t>(sp + 1);
            }
            if (vary_dport) {
                udp->dst_port = rte_cpu_to_be_16(dp);
                dp = dp >= dp_hi ? dp_lo : static_cast<uint16_t>(dp + 1);
            }
        } else {
            auto *snp = reinterpret_cast<uint32_t *>(d + l4_off + 4);
            *snp = rte_cpu_to_be_32(sn++);
        }
        if (fdata) {
            /* [chunk index][file bytes], the last chunk zero-padded. */
            const uint64_t c = chunk++;
            uint8_t *cp = d + chunk_off;
            memcpy(cp, &c, sizeof(c));
            const uint64_t off = c * fchunk;
            const uint64_t n = fsize - off < fchunk ? fsize - off : fchunk;
            rte_memcpy(cp + 8, fdata + off, n);
            if (n < fchunk)
                memset(cp + 8 + n, 0, fchunk - n);
        }
    }
    t.next_chunk = chunk;
    t.next_seq = seq;
    t.next_sport = sp;
    t.next_dport = dp;
    t.next_sn = sn;
    w->prng = prng;
    w->stats.req_lost += lost;
}

/* --esp-encap hw: describe one frame that came back. The responder only swaps
 * the addresses, so this is the frame as the NIC built it on the way out. */
static __attribute__((noinline, cold)) void log_rx_frame(const WorkerCtx *w, const struct rte_mbuf *m)
{
    const auto *eth = rte_pktmbuf_mtod(m, const struct rte_ether_hdr *);
    const auto *ip = reinterpret_cast<const struct rte_ipv4_hdr *>(eth + 1);
    if (m->data_len < sizeof(*eth) + sizeof(*ip) + 8 || eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4)) {
        APP_LOG_INFO("lcore %u: response of %u B, not IPv4", w->lcore_id, m->pkt_len);
        return;
    }
    const auto *esp = reinterpret_cast<const rte_be32_t *>(reinterpret_cast<const uint8_t *>(ip) +
                                                            ((ip->version_ihl & 0x0f) << 2));
    APP_LOG_INFO("lcore %u: response of %u B (+4 FCS): IPv4 protocol %u, total length %u, SPI 0x%x, SN %u",
                 w->lcore_id, m->pkt_len, ip->next_proto_id, rte_be_to_cpu_16(ip->total_length),
                 rte_be_to_cpu_32(esp[0]), rte_be_to_cpu_32(esp[1]));
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
    const TrafficKind kind = frame_kind(cfg);
    const uint64_t rps = cfg.rps;
    const bool copy_template = cfg.tx_fill == TxFill::Copy;
    const bool record = t.rec_data != nullptr;
    const bool sn_check = t.sn_check;
    struct rte_mempool *const tx_pool = t.tx_pool;
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
                if (unlikely(t.rx_show)) {
                    t.rx_show--;
                    log_rx_frame(w, pkts[i]);
                }
                if (record)
                    record_chunk(t, s, pkts[i], kind);
                if (sn_check)
                    check_esp_sn(t, s, pkts[i]);
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
        if (g_file.data) {
            /* Each lcore sends the file once, then only receives. */
            const uint64_t left = g_file.chunks - t.next_chunk;
            if (batch > left)
                batch = static_cast<uint32_t>(left);
            if (left == 0 && !t.file_sent_logged) {
                t.file_sent_logged = true;
                APP_LOG_INFO("lcore %u: file sent (%lu chunks, %lu refused and re-sent), now only receiving",
                             lcore_id, static_cast<unsigned long>(g_file.chunks),
                             static_cast<unsigned long>(s.tx_drops));
            }
        }
        if (batch) {
            const int got = copy_template
                ? rte_pktmbuf_alloc_bulk(tx_pool, pkts, batch)
                : rte_mempool_get_bulk(tx_pool, reinterpret_cast<void **>(pkts), batch);
            if (unlikely(got != 0)) {
                s.tx_drops += batch;
            } else {
                const uint64_t send_tsc = rte_rdtsc();
                if (kind == TrafficKind::Udp) {
                    if (copy_template)
                        fill_requests<TrafficKind::Udp, true>(t, cfg, w, pkts, batch, send_tsc);
                    else
                        fill_requests<TrafficKind::Udp, false>(t, cfg, w, pkts, batch, send_tsc);
                } else {
                    if (copy_template)
                        fill_requests<TrafficKind::Esp, true>(t, cfg, w, pkts, batch, send_tsc);
                    else
                        fill_requests<TrafficKind::Esp, false>(t, cfg, w, pkts, batch, send_tsc);
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
                    /* The refused chunks go out again with the same SNs, so the
                     * stream on the wire stays chunk i = SN i. */
                    if (g_file.data) {
                        t.next_chunk -= batch - sent;
                        t.next_sn -= batch - sent;
                    }
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
    if (record)
        dump_record(cfg, *w, t);
    return 0;
}
