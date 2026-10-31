/* SPDX-License-Identifier: BSD-3-Clause
 *
 * musq-testapp: receive-then-send-back DPDK test application for the mu_sq
 * (multi-user SQ, "PROM") mlx5 PMD feature and its DOCA Flow steering.
 * Shared types used by every module.
 */
#pragma once

#include <rte_common.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_lcore.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#define RTE_LOGTYPE_MUSQ RTE_LOGTYPE_USER2
#define APP_LOG(level, ...) RTE_LOG(level, MUSQ, __VA_ARGS__)
#define APP_LOG_INFO(fmt, ...) APP_LOG(INFO, fmt "\n", ##__VA_ARGS__)
#define APP_LOG_WARN(fmt, ...) APP_LOG(WARNING, fmt "\n", ##__VA_ARGS__)
#define APP_LOG_ERR(fmt, ...) APP_LOG(ERR, fmt "\n", ##__VA_ARGS__)

/* Array capacity for RX/TX bursts; the configured bursts must be <= this. */
constexpr uint16_t MAX_BURST = 512;
/* mlx5 mu_sq eMPW slot size (MLX5_MU_EMPW_PKTS in drivers/net/mlx5/mlx5.h). */
constexpr uint16_t MUSQ_EMPW_PKTS = 32;
constexpr uint16_t MUSQ_MAX_GROUP = 128; /* MLX5_MU_SQ_LOG_GRP_MAX_SIZE = 7 */
/* Most mu_sq groups (shared rings) on the port; the NIC's hash pipe needs a
 * power of 2 of them. */
constexpr unsigned MUSQ_MAX_GROUPS = 64;
/* Most TX queues (mu_sq lanes) one responder lcore can post to (--tx-lanes). */
constexpr unsigned MAX_TX_LANES = 8;
/* Most RX queues one responder lcore polls (--rx-queues). */
constexpr unsigned MAX_RX_QUEUES = 8;
/* Most worker lcores one software-distributor master deals to (--masters). */
constexpr unsigned MAX_DIST_WORKERS = 16;

enum class DocaMode { None, MetaRr, EspRr };
enum class TxMode { Immediate, Accumulate };
enum class SwapMode { L2L3, L2, None };
enum class Role { Responder, Requester };
enum class WorkKind { Fixed, Bimodal, Exp, Header };
enum class XstatsMode { Filtered, All, Final };
enum class TrafficKind { Udp, Esp };
/* Requester: how responses reach the lcore that owns the request. */
enum class RxSteer { Flow, Rss, None };
/* Requester: how a request frame is produced from the template. */
enum class TxFill { Prefill, Copy };
/* Software distributor (--masters): how a master restores the arrival order
 * (rr: deal and collect round robin, the order is implicit; seqn: sequence
 * numbers and a reorder buffer), which worker gets the next chunk (rr, or lb =
 * the one with the fewest packets in flight), and an lcore's part in it. */
enum class DistOrder { Rr, Seqn };
enum class DistDeal { Rr, Lb };
enum class DistRole { None, Master, Worker, TxMaster };

struct WorkSpec {
    WorkKind kind = WorkKind::Fixed;
    uint64_t lo_ns = 0;   /* fixed: the value; bimodal: low; exp: mean */
    uint64_t hi_ns = 0;   /* bimodal: high */
    uint32_t pct_hi = 0;  /* bimodal: percent of packets that get hi_ns */
};

struct AppConfig {
    std::string pci = "0000:17:00.1";
    std::string devargs;            /* extra mlx5 devargs */
    int musq_group = -1;            /* lanes per group: -1 = auto (TX queues / groups), 0 = off */
    /* mu_sq groups on the port, each its own shared ring: group g = TX queues
     * g*N .. g*N+N-1 (N = musq_group_eff()); the PMD derives the count from
     * the TX queues and the group size. In a DOCA mode the NIC hashes a flow
     * (outer IPv4 src + dst) to one group and round-robins inside it. */
    unsigned musq_groups = 1;
    /* --hash-probe: log at start which group the flows src..src+n-1 -> dst
     * land in (the requester's src_ip + lcore index convention). */
    std::string hash_probe_src, hash_probe_dst;
    unsigned hash_probe_n = 0;
    std::vector<unsigned> lcores;   /* worker lcores */
    unsigned main_lcore = 0;
    DocaMode doca_mode = DocaMode::EspRr;
    uint16_t doca_group = 1;
    uint16_t rx_burst = 32;
    uint16_t tx_burst = 32;
    TxMode tx_mode = TxMode::Accumulate;
    uint64_t tx_drain_us = 100;
    WorkSpec work;
    SwapMode swap = SwapMode::L2L3;
    uint16_t rxd = 1024;
    uint16_t txd = 1024;
    bool per_lcore_pool = true;
    bool promisc = true;
    double duration_s = 0;
    double warmup_s = 0;
    double stats_interval_s = 1.0;
    std::string stats_csv;
    XstatsMode xstats = XstatsMode::Filtered;
    Role role = Role::Responder;
    std::string file_prefix = "musq";
    int log_level = 7;
    std::string eal_args;
    bool dry_run = false;

    /* requester role (phase 2) */
    uint64_t rps = 1000000;             /* per requester lcore */
    WorkSpec service_time;              /* ns spec, sent on the wire in us */
    TrafficKind traffic = TrafficKind::Esp;
    uint16_t frame_size = 92;           /* on the wire incl. FCS */
    std::string dst_mac = "f4:20:4d:24:44:86";
    std::string src_mac;                /* empty = port MAC */
    std::string src_ip = "1.1.1.1";        /* base; flow/rss steer: + worker index per lcore */
    std::string dst_ip = "2.2.2.2";
    uint16_t sport_lo = 1025, sport_hi = 1040;
    uint16_t dport_lo = 12345, dport_hi = 12345;
    uint32_t tracker_size = 1u << 20;   /* outstanding-request slots per lcore, power of 2 */
    RxSteer rx_steer = RxSteer::Flow;   /* flow: per-lcore src IP + rte_flow dst IP -> owner queue */
    TxFill tx_fill = TxFill::Prefill;   /* prefill: TX-only pool, template written once per mbuf */
    /* --traffic esp: false = the lcores write the ESP header and sequence
     * number themselves (nothing is encrypted); true = they send plain UDP
     * frames and the NIC encapsulates and encrypts them (IPsec packet offload,
     * see esp_hw.hpp), with the sequence number counted by the NIC. */
    bool esp_hw = false;
    /* --esp-encap hw: one IPsec SA, i.e. one SPI and one NIC sequence-number
     * counter, per lcore (true; SPI = esp_spi + lcore index, which the sw
     * encap writes as well) or one SA for the whole port (false, the original
     * mode: one SN stream over all lcores). */
    bool esp_sa_per_lcore = true;
    uint32_t esp_spi = 0x1000;
    /* Explicit source IPv4 per lcore (as many as lcores) instead of
     * src_ip + lcore index: to pick flows that land in distinct mu_sq groups
     * of the responder (its --hash-probe). */
    std::vector<std::string> src_ips;
    /* --traffic esp: check that each lcore's responses come back with
     * increasing ESP sequence numbers (the SPI and SN are clear text, also
     * with hw encap), counting reorders and gaps: the order proof. */
    bool esp_sn_check = true;
    /* Stream correctness test: every requester lcore sends this file once, as
     * its own flow, one chunk per request ([8 B chunk index][file bytes]), and
     * stops sending when it is done. */
    std::string tx_file;
    /* Record what comes back on each requester lcore's queue, in arrival
     * order, to <prefix>.w<worker>.{data,idx,json} at exit (see
     * scripts/stream_compare.py). */
    std::string rx_record;

    /* Packets per TX WQE: 32 = multi-packet WQEs (mu_sq 32-packet slots of 9
     * WQEBBs, or eMPW without mu_sq); 1 = one packet per WQE (mu_sq 1-packet
     * slots of 1 WQEBB, which is a separate PMD build picked by scripts/run.sh,
     * or txq_mpw_en=0 without mu_sq). */
    unsigned pkts_per_wqe = 32;

    /* Responder, one worker lcore: TX queues (mu_sq lanes) that lcore posts
     * to, dealing the packets to them in turn. 1 = one TX queue per worker. */
    unsigned tx_lanes = 1;

    /* Responder, --doca-mode none: RX queues per worker lcore, polled in
     * turn; RSS spreads the traffic over all workers' queues. TX is not
     * affected (one TX queue per worker, or --tx-lanes). */
    unsigned rx_queues = 1;

    /* Responder with mu_sq: the PMD rings a lane's doorbell once every this
     * many WQEs (mu_sq slots) the lane posts, through the mlx5 devarg
     * mu_sq_db_wqes. 0 = the PMD's own rule, one doorbell per send pass. */
    unsigned db_wqes = 0;

    /* Software distributor (responder): master lcores that receive, deal the
     * packets to the worker lcores (--lcores, split evenly among the masters in
     * lcore order) over rings, take them back in arrival order and send them
     * on their own TX queue; the workers only process packets. Empty = off,
     * every lcore in --lcores runs the whole path itself (see dist.hpp). */
    std::vector<unsigned> masters;
    /* --tx-masters: one lcore per master, paired in lcore order, that takes
     * over the collecting and sending half of that master (a two-stage
     * pipeline); the master then only receives and deals. Empty = off. */
    std::vector<unsigned> tx_masters;
    DistOrder dist_order = DistOrder::Rr;
    DistDeal dist_deal = DistDeal::Rr;
    unsigned dist_chunk = 32;   /* consecutive packets dealt to one worker before moving on */
    unsigned dist_ring = 2048;  /* entries of each master <-> worker ring (power of 2) */
    /* --dist-self: the master processes up to this many packets per deal
     * cycle itself (one more slot in the round robin) whenever a worker has
     * at least dist_self_backlog packets outstanding, i.e. the workers are
     * the limit and the master has cycles to spare. 0 = off. */
    unsigned dist_self = 0;
    unsigned dist_self_backlog = 128;  /* tokens waiting in some worker's input ring */
    unsigned dist_self_idle = 5;       /* percent of the master's recent passes that were idle */

    /* per-epoch lane status log (responder; needs the prom-dpdk mlx5 PMD) */
    uint64_t lane_epoch_us = 0;         /* 0 = off */
    uint32_t lane_epoch_max = 120000;   /* records per lane */
    std::string lane_epoch_out = "lane_epoch.csv";

    unsigned nb_workers() const { return static_cast<unsigned>(lcores.size()); }
    bool doca_enabled() const { return doca_mode != DocaMode::None; }
    bool dist() const { return !masters.empty(); }
    bool dist_split() const { return dist() && !tx_masters.empty(); }
    unsigned nb_masters() const { return static_cast<unsigned>(masters.size()); }
    unsigned workers_per_master() const { return dist() ? nb_workers() / nb_masters() : 0; }
    /* Lcores that run a loop: the workers, plus the masters (and their TX
     * masters) in dist mode. */
    unsigned nb_lcores() const { return nb_workers() + nb_masters() + static_cast<unsigned>(tx_masters.size()); }
    /* Queue owners: the masters in dist mode (each with its RX queues and one
     * TX queue, the latter run by its TX master with --tx-masters), else the
     * workers. */
    unsigned nb_queue_lcores() const { return dist() ? nb_masters() : nb_workers(); }
    unsigned nb_rx_queues() const { return nb_queue_lcores() * rx_queues; }
    unsigned nb_tx_queues() const { return nb_queue_lcores() * tx_lanes; }
    /* Effective mu_sq group size (lanes per group): 0 = feature off. Auto is
     * one lane per TX queue, the queues split evenly over the groups. With
     * --masters (the software alternative) auto means off; an explicit group
     * there is refused. */
    unsigned musq_group_eff() const {
        if (musq_group < 0)
            return dist() || musq_groups == 0 ? 0 : nb_tx_queues() / musq_groups;
        return static_cast<unsigned>(musq_group);
    }
    /* RX queues per steering group: what the DOCA pipeline round-robins over
     * inside a group (= the lanes of a group when mu_sq is on). */
    unsigned group_rx_queues() const { return musq_groups == 0 ? 0 : nb_rx_queues() / musq_groups; }
};

/* korba request header (request_tracker.hpp), for the phase-2 requester and
 * the `--work header` mode: the request carries its own processing time. */
struct __attribute__((packed)) request_header {
    uint8_t magic_number;
    uint64_t request_id;
    uint32_t processing_time_us;
    uint8_t reserved[5];
};
constexpr uint8_t REQUEST_MAGIC = 0xAB;
constexpr uint8_t RESPONSE_MAGIC = 0xAC;

/* Per-worker counters. Written only by the owning lcore, read by the main
 * lcore (relaxed loads); one cache line per worker so lanes never false-share. */
struct alignas(RTE_CACHE_LINE_SIZE) WorkerStats {
    uint64_t rx_pkts = 0;
    uint64_t tx_pkts = 0;
    uint64_t tx_drops = 0;        /* refused by rte_eth_tx_burst and freed */
    uint64_t rx_bursts = 0;       /* non-empty rte_eth_rx_burst calls */
    uint64_t rx_full_bursts = 0;  /* nb_rx == rx_burst */
    uint64_t non_ipv4 = 0;
    uint64_t work_cycles = 0;     /* cycles spent in the mock work spin */
    uint64_t cycles_used = 0;     /* loop iterations that handled packets */
    uint64_t cycles_wasted = 0;   /* empty polls */
    uint64_t loop_start_tsc = 0;
    /* requester role */
    uint64_t req_sent = 0;        /* requests handed to the NIC */
    uint64_t resp_matched = 0;    /* responses matched to an outstanding request (on the RX lcore) */
    uint64_t resp_unmatched = 0;  /* response magic but no matching slot (late, duplicate, stale) */
    uint64_t req_lost = 0;        /* slot reused while its request was still outstanding */
    uint64_t tx_batch_capped = 0; /* pacing asked for more than MAX_BURST in one iteration */
    uint64_t rec_chunks = 0;      /* --rx-record: chunks recorded */
    uint64_t rec_bad = 0;         /* --rx-record: frames that carry no chunk */
    uint64_t rec_overflow = 0;    /* --rx-record: chunks beyond the record buffer */
    /* ESP SN order check (requester, --traffic esp; requester.cpp check_esp_sn) */
    uint64_t sn_reorder = 0;      /* responses whose SN did not exceed the previous one: order broken */
    uint64_t sn_gap_pkts = 0;     /* SNs skipped between consecutive responses (lost, or not back yet) */
    uint64_t sn_max_back = 0;     /* largest backward SN step seen */
    uint64_t spi_foreign = 0;     /* ESP responses carrying another SPI than this lcore's */
    /* software distributor (--masters) */
    uint64_t dealt = 0;           /* master: packets handed to its workers */
    uint64_t collected = 0;       /* master (or TX master): packets taken back from them */
    uint64_t rx_held = 0;         /* master: loop passes that skipped RX for lack of ring room */
    uint64_t stalls = 0;          /* master (or TX master): collect passes blocked by one worker while others' returns wait */
    uint64_t ring_retries = 0;    /* ring enqueues that had to be retried (0 by construction) */
    uint64_t self_pkts = 0;       /* master: packets it processed itself (--dist-self) */
};

struct RequestTracker;
struct hdr_histogram;
struct LaneEpochLog;
struct DistMaster;
struct DistWorker;

struct alignas(RTE_CACHE_LINE_SIZE) WorkerCtx {
    unsigned lcore_id = 0;
    uint16_t worker_idx = 0;
    uint16_t port_id = 0;
    uint16_t queue_id = 0;                 /* first RX queue; with one RX queue and one TX lane also the TX queue */
    uint16_t nb_rx_queues = 1;             /* RX queues queue_id .. queue_id + n - 1, polled in turn */
    /* --musq-groups: the group of this lcore's queues (group g = queues
     * g*N..g*N+N-1) and the lane in it; cold, for logs and the stats table. */
    uint16_t musq_group = 0;
    uint16_t lane_in_group = 0;
    Role role = Role::Responder;
    struct rte_mempool *pool = nullptr;
    /* Responder TX: one buffer per TX queue (lane) this lcore posts to. */
    uint16_t nb_tx_lanes = 1;
    uint16_t next_lane = 0;                /* --tx-lanes > 1: lane of the next packet */
    struct TxBuf {
        struct rte_mbuf *m[MAX_BURST];
        uint16_t len = 0;
        uint16_t queue_id = 0;
        uint64_t tx_pkts = 0;              /* --tx-lanes > 1: this lane's share of stats.tx_pkts */
        uint64_t tx_drops = 0;
        uint64_t db_seen = 0;              /* --db-wqes: the lane's TX count at the last drain tick */
    } txbuf[MAX_TX_LANES];
    uint64_t prng = 0;
    RequestTracker *tracker = nullptr;     /* requester: this lcore's outstanding requests */
    struct hdr_histogram *rtt_hist = nullptr; /* requester: RTTs of responses received on this lcore (us) */
    LaneEpochLog *epoch_log[MAX_TX_LANES] = {}; /* responder: per-epoch status of each TX lane, written by this lcore only */
    /* Software distributor (--masters): this lcore's part, None outside the mode. */
    DistRole dist_role = DistRole::None;
    uint16_t master_idx = 0;               /* master: its index among the masters; worker, TX master: its master's */
    DistMaster *dm = nullptr;              /* master and its TX master: links to the workers and the ordering state */
    DistWorker *dw = nullptr;              /* worker: its two rings */
    WorkerStats stats;
};

struct AppState {
    AppConfig cfg;
    uint16_t port_id = 0;
    unsigned nb_workers = 0;
    uint64_t tsc_hz = 0;
    WorkerCtx workers[RTE_MAX_LCORE];
    int lcore_to_worker[RTE_MAX_LCORE];
    std::vector<struct rte_mempool *> pools;
    volatile bool force_quit = false;
    std::atomic<bool> measuring{false};
};

extern AppState g_app;

static inline bool is_pow2(uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }
static inline unsigned log2_pow2(unsigned v) { return static_cast<unsigned>(__builtin_ctz(v)); }
