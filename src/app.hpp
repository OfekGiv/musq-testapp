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

enum class DocaMode { None, MetaRr, EspRr };
enum class TxMode { Immediate, Accumulate };
enum class SwapMode { L2L3, L2, None };
enum class Role { Responder, Requester };
enum class WorkKind { Fixed, Bimodal, Exp, Header };
enum class XstatsMode { Filtered, All, Final };
enum class TrafficKind { Udp, Esp };

struct WorkSpec {
    WorkKind kind = WorkKind::Fixed;
    uint64_t lo_ns = 0;   /* fixed: the value; bimodal: low; exp: mean */
    uint64_t hi_ns = 0;   /* bimodal: high */
    uint32_t pct_hi = 0;  /* bimodal: percent of packets that get hi_ns */
};

struct AppConfig {
    std::string pci = "0000:17:00.1";
    std::string devargs;            /* extra mlx5 devargs */
    int musq_group = -1;            /* -1 = auto (nb workers), 0 = off */
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
    std::string src_ip = "1.1.1.1";
    std::string dst_ip = "2.2.2.2";
    uint16_t sport_lo = 1025, sport_hi = 1040;
    uint16_t dport_lo = 12345, dport_hi = 12345;
    uint32_t tracker_size = 1u << 20;   /* outstanding-request slots per lcore, power of 2 */

    unsigned nb_workers() const { return static_cast<unsigned>(lcores.size()); }
    bool doca_enabled() const { return doca_mode != DocaMode::None; }
    /* Effective mu_sq group size: 0 = feature off. */
    unsigned musq_group_eff() const {
        return musq_group < 0 ? nb_workers() : static_cast<unsigned>(musq_group);
    }
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
};

struct RequestTracker;
struct hdr_histogram;

struct alignas(RTE_CACHE_LINE_SIZE) WorkerCtx {
    unsigned lcore_id = 0;
    uint16_t worker_idx = 0;
    uint16_t port_id = 0;
    uint16_t queue_id = 0;
    Role role = Role::Responder;
    struct rte_mempool *pool = nullptr;
    struct {
        struct rte_mbuf *m[MAX_BURST];
        uint16_t len = 0;
    } txbuf;
    uint64_t prng = 0;
    RequestTracker *tracker = nullptr;     /* requester: this lcore's outstanding requests */
    struct hdr_histogram *rtt_hist = nullptr; /* requester: RTTs of responses received on this lcore (us) */
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
