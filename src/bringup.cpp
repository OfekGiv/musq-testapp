#include "bringup.hpp"
#include "config.hpp"

#include <rte_ethdev.h>
#include <rte_cycles.h>
#include <rte_random.h>
#include <rte_malloc.h>

#include <cstdlib>
#include <cstring>

#if ENABLE_DOCA
#include "doca_pipelines_shim.h"
#endif

int doca_probe(const AppConfig &cfg)
{
#if ENABLE_DOCA
    const std::string devargs = musq_devargs(cfg);
    APP_LOG_INFO("EAL ok; probing DOCA DPDK bridge for PCI %s (devargs '%s', mode %s)",
                 cfg.pci.c_str(), devargs.c_str(), doca_mode_name(cfg.doca_mode));
    if (l3fwd_doca_pipelines_dpdk_probe(cfg.pci.c_str(), devargs.c_str()) < 0) {
        APP_LOG_ERR("DOCA DPDK bridge probe failed for %s", cfg.pci.c_str());
        return -1;
    }
    const uint16_t n = rte_eth_dev_count_avail();
    if (n != 1) {
        APP_LOG_ERR("expected exactly one ethdev after the DOCA probe, found %u "
                    "(the DOCA pipeline SDK hard-codes port 0)", n);
        return -1;
    }
    char name[RTE_ETH_NAME_MAX_LEN] = {};
    rte_eth_dev_get_name_by_port(0, name);
    APP_LOG_INFO("DOCA probe ok: ethdev port 0 = %s", name);
    return 0;
#else
    (void)cfg;
    APP_LOG_ERR("built without DOCA support");
    return -1;
#endif
}

int doca_init(const AppConfig &cfg, unsigned nb_workers)
{
#if ENABLE_DOCA
    APP_LOG_INFO("installing DOCA %s pipeline: %u queues, steering group %u",
                 doca_mode_name(cfg.doca_mode), nb_workers, cfg.doca_group);
    if (l3fwd_doca_pipelines_init(static_cast<uint16_t>(nb_workers), cfg.doca_group) < 0) {
        APP_LOG_ERR("DOCA pipeline init failed");
        return -1;
    }
    return 0;
#else
    (void)cfg;
    (void)nb_workers;
    return -1;
#endif
}

void doca_cleanup()
{
#if ENABLE_DOCA
    l3fwd_doca_pipelines_cleanup();
#endif
}

int setup_workers(AppState &st)
{
    for (unsigned i = 0; i < RTE_MAX_LCORE; i++)
        st.lcore_to_worker[i] = -1;
    unsigned idx = 0;
    unsigned lcore;
    RTE_LCORE_FOREACH_WORKER(lcore) {
        WorkerCtx &w = st.workers[idx];
        w.lcore_id = lcore;
        w.worker_idx = static_cast<uint16_t>(idx);
        w.port_id = st.port_id;
        w.queue_id = static_cast<uint16_t>(idx * st.cfg.rx_queues);
        w.nb_rx_queues = static_cast<uint16_t>(st.cfg.rx_queues);
        w.role = st.cfg.role;
        w.nb_tx_lanes = static_cast<uint16_t>(st.cfg.tx_lanes);
        w.next_lane = 0;
        for (unsigned l = 0; l < st.cfg.tx_lanes; l++) {
            w.txbuf[l].len = 0;
            w.txbuf[l].queue_id = static_cast<uint16_t>(idx * st.cfg.tx_lanes + l);
        }
        w.prng = rte_rand() | 1;
        st.lcore_to_worker[lcore] = static_cast<int>(idx);
        idx++;
    }
    st.nb_workers = idx;
    if (idx != st.cfg.nb_workers()) {
        APP_LOG_ERR("EAL reports %u worker lcores, config has %u", idx, st.cfg.nb_workers());
        return -1;
    }
    const int nic_socket = rte_eth_dev_socket_id(st.port_id);
    for (unsigned i = 0; i < st.nb_workers; i++) {
        const int s = static_cast<int>(rte_lcore_to_socket_id(st.workers[i].lcore_id));
        if (nic_socket >= 0 && s != nic_socket)
            APP_LOG_WARN("lcore %u is on socket %d but port %u is on socket %d",
                         st.workers[i].lcore_id, s, st.port_id, nic_socket);
    }
    return 0;
}

static unsigned pool_size_for(const AppConfig &cfg, unsigned nb_workers)
{
    /* Per worker: its RX rings + TX rings in flight + bursts in the app; then
     * some headroom, rounded to 2^n - 1 (optimum for rte_mempool). */
    uint64_t n = static_cast<uint64_t>(nb_workers) *
                     (cfg.rx_queues * cfg.rxd + cfg.tx_lanes * (cfg.txd + MAX_BURST) + MAX_BURST) +
                 static_cast<uint64_t>(nb_workers) * RTE_MEMPOOL_CACHE_MAX_SIZE;
    n = n + n / 4;
    if (n < 8192)
        n = 8192;
    uint64_t p = 1;
    while (p - 1 < n)
        p <<= 1;
    return static_cast<unsigned>(p - 1);
}

int create_pools(AppState &st)
{
    int socket = rte_eth_dev_socket_id(st.port_id);
    if (socket < 0)
        socket = static_cast<int>(rte_socket_id());
    const AppConfig &cfg = st.cfg;
    if (cfg.per_lcore_pool) {
        const unsigned n = pool_size_for(cfg, 1);
        for (unsigned i = 0; i < st.nb_workers; i++) {
            char name[RTE_MEMPOOL_NAMESIZE];
            snprintf(name, sizeof(name), "mbuf_pool_lc%u", st.workers[i].lcore_id);
            struct rte_mempool *p = rte_pktmbuf_pool_create(name, n, RTE_MEMPOOL_CACHE_MAX_SIZE, 0,
                                                            RTE_MBUF_DEFAULT_BUF_SIZE, socket);
            if (p == nullptr) {
                APP_LOG_ERR("cannot create mempool %s (%u mbufs): %s", name, n, rte_strerror(rte_errno));
                return -1;
            }
            st.workers[i].pool = p;
            st.pools.push_back(p);
            APP_LOG_INFO("mempool %s: %u mbufs on socket %d", name, n, socket);
        }
    } else {
        const unsigned n = pool_size_for(cfg, st.nb_workers);
        struct rte_mempool *p = rte_pktmbuf_pool_create("mbuf_pool", n, RTE_MEMPOOL_CACHE_MAX_SIZE, 0,
                                                        RTE_MBUF_DEFAULT_BUF_SIZE, socket);
        if (p == nullptr) {
            APP_LOG_ERR("cannot create mempool (%u mbufs): %s", n, rte_strerror(rte_errno));
            return -1;
        }
        for (unsigned i = 0; i < st.nb_workers; i++)
            st.workers[i].pool = p;
        st.pools.push_back(p);
        APP_LOG_INFO("mempool mbuf_pool: %u mbufs on socket %d (shared)", n, socket);
    }
    return 0;
}

/* l3fwd check_all_ports_link_status, single port. */
static void wait_link_up(uint16_t port_id)
{
    constexpr int CHECK_INTERVAL_MS = 100;
    constexpr int MAX_CHECK = 90;
    struct rte_eth_link link;
    for (int i = 0; i < MAX_CHECK; i++) {
        memset(&link, 0, sizeof(link));
        const int ret = rte_eth_link_get_nowait(port_id, &link);
        if (ret < 0) {
            APP_LOG_WARN("port %u link get failed: %s", port_id, rte_strerror(-ret));
            return;
        }
        if (link.link_status == RTE_ETH_LINK_UP) {
            char text[RTE_ETH_LINK_MAX_STR_LEN];
            rte_eth_link_to_str(text, sizeof(text), &link);
            APP_LOG_INFO("port %u %s", port_id, text);
            return;
        }
        rte_delay_ms(CHECK_INTERVAL_MS);
    }
    APP_LOG_WARN("port %u link is still down after %d ms", port_id, CHECK_INTERVAL_MS * MAX_CHECK);
}

int configure_port(AppState &st)
{
    const AppConfig &cfg = st.cfg;
    const uint16_t port = st.port_id;
    const uint16_t nq = static_cast<uint16_t>(st.nb_workers * cfg.rx_queues);
    const uint16_t ntxq = static_cast<uint16_t>(st.nb_workers * cfg.tx_lanes);
    struct rte_eth_dev_info dev_info;
    int ret = rte_eth_dev_info_get(port, &dev_info);
    if (ret != 0) {
        APP_LOG_ERR("rte_eth_dev_info_get(%u): %s", port, strerror(-ret));
        return -1;
    }
    if (nq > dev_info.max_rx_queues || ntxq > dev_info.max_tx_queues) {
        APP_LOG_ERR("port %u supports %u rx / %u tx queues, need %u / %u", port,
                    dev_info.max_rx_queues, dev_info.max_tx_queues, nq, ntxq);
        return -1;
    }

    /* l3fwd port_conf (examples/l3fwd/main.c). */
    struct rte_eth_conf conf;
    memset(&conf, 0, sizeof(conf));
    conf.rxmode.mq_mode = nq > 1 ? RTE_ETH_MQ_RX_RSS : RTE_ETH_MQ_RX_NONE;
    conf.rxmode.offloads = RTE_ETH_RX_OFFLOAD_CHECKSUM & dev_info.rx_offload_capa;
    conf.rx_adv_conf.rss_conf.rss_key = nullptr;
    /* Hash on L4 ports too (korba), not only IP (l3fwd): a single IP pair with
     * many UDP ports then spreads over the queues in plain RSS mode. In DOCA
     * modes the pipeline steers explicitly and this is moot. */
    conf.rx_adv_conf.rss_conf.rss_hf =
        (RTE_ETH_RSS_IP | RTE_ETH_RSS_UDP | RTE_ETH_RSS_TCP) & dev_info.flow_type_rss_offloads;
    conf.txmode.mq_mode = RTE_ETH_MQ_TX_NONE;
    if (dev_info.tx_offload_capa & RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE)
        conf.txmode.offloads |= RTE_ETH_TX_OFFLOAD_MBUF_FAST_FREE;

    APP_LOG_INFO("configuring port %u: %u rx + %u tx queues, rss_hf=%#lx rx_offloads=%#lx tx_offloads=%#lx",
                 port, nq, ntxq, static_cast<unsigned long>(conf.rx_adv_conf.rss_conf.rss_hf),
                 static_cast<unsigned long>(conf.rxmode.offloads),
                 static_cast<unsigned long>(conf.txmode.offloads));
    ret = rte_eth_dev_configure(port, nq, ntxq, &conf);
    if (ret < 0) {
        APP_LOG_ERR("rte_eth_dev_configure(%u): %s", port, rte_strerror(-ret));
        return -1;
    }
    uint16_t rxd = cfg.rxd, txd = cfg.txd;
    ret = rte_eth_dev_adjust_nb_rx_tx_desc(port, &rxd, &txd);
    if (ret < 0) {
        APP_LOG_ERR("rte_eth_dev_adjust_nb_rx_tx_desc(%u): %s", port, rte_strerror(-ret));
        return -1;
    }
    if (rxd != cfg.rxd || txd != cfg.txd)
        APP_LOG_WARN("descriptor counts adjusted by the PMD: rxd %u -> %u, txd %u -> %u",
                     cfg.rxd, rxd, cfg.txd, txd);

    struct rte_ether_addr mac;
    rte_eth_macaddr_get(port, &mac);
    char macs[RTE_ETHER_ADDR_FMT_SIZE];
    rte_ether_format_addr(macs, sizeof(macs), &mac);
    APP_LOG_INFO("port %u MAC %s", port, macs);

    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        const unsigned socket = rte_lcore_to_socket_id(w.lcore_id);
        struct rte_eth_txconf txconf = dev_info.default_txconf;
        txconf.offloads = conf.txmode.offloads;
        for (unsigned l = 0; l < w.nb_tx_lanes; l++) {
            ret = rte_eth_tx_queue_setup(port, w.txbuf[l].queue_id, txd, socket, &txconf);
            if (ret < 0) {
                APP_LOG_ERR("rte_eth_tx_queue_setup(%u, q%u): %s", port, w.txbuf[l].queue_id, rte_strerror(-ret));
                return -1;
            }
        }
        struct rte_eth_rxconf rxconf = dev_info.default_rxconf;
        rxconf.offloads = conf.rxmode.offloads;
        for (unsigned k = 0; k < w.nb_rx_queues; k++) {
            const uint16_t q = static_cast<uint16_t>(w.queue_id + k);
            ret = rte_eth_rx_queue_setup(port, q, rxd, socket, &rxconf, w.pool);
            if (ret < 0) {
                APP_LOG_ERR("rte_eth_rx_queue_setup(%u, q%u): %s", port, q, rte_strerror(-ret));
                return -1;
            }
        }
        if (w.nb_rx_queues > 1)
            APP_LOG_INFO("lcore %u: port %u rx queues %u..%u (polled in turn), tx queue(s) %u..%u, pool %s, socket %u",
                         w.lcore_id, port, w.queue_id, w.queue_id + w.nb_rx_queues - 1, w.txbuf[0].queue_id,
                         w.txbuf[w.nb_tx_lanes - 1].queue_id, w.pool->name, socket);
        else if (w.nb_tx_lanes == 1)
            APP_LOG_INFO("lcore %u: port %u queue %u (rx+tx), pool %s, socket %u",
                         w.lcore_id, port, w.queue_id, w.pool->name, socket);
        else
            APP_LOG_INFO("lcore %u: port %u rx queue %u, tx queues %u..%u (%u lanes dealt in turn), pool %s, socket %u",
                         w.lcore_id, port, w.queue_id, w.txbuf[0].queue_id,
                         w.txbuf[w.nb_tx_lanes - 1].queue_id, w.nb_tx_lanes, w.pool->name, socket);
    }

    ret = rte_eth_dev_start(port);
    if (ret < 0) {
        APP_LOG_ERR("rte_eth_dev_start(%u): %s", port, rte_strerror(-ret));
        return -1;
    }
    /* The TX routine the PMD selected at start: on mlx5 "Enhanced MPW ..." or,
     * with txq_mpw_en=0, "No MPW ..." (one SEND WQE per packet). */
    struct rte_eth_burst_mode tx_mode;
    if (rte_eth_tx_burst_mode_get(port, 0, &tx_mode) == 0)
        APP_LOG_INFO("port %u TX burst mode: %s", port, tx_mode.info);
    if (cfg.promisc) {
        ret = rte_eth_promiscuous_enable(port);
        if (ret != 0) {
            APP_LOG_ERR("rte_eth_promiscuous_enable(%u): %s", port, rte_strerror(-ret));
            return -1;
        }
    }
    wait_link_up(port);
    return 0;
}

void stop_port(AppState &st)
{
    APP_LOG_INFO("stopping port %u", st.port_id);
    const int ret = rte_eth_dev_stop(st.port_id);
    if (ret != 0)
        APP_LOG_WARN("rte_eth_dev_stop(%u): %s", st.port_id, rte_strerror(-ret));
    rte_eth_dev_close(st.port_id);
}

void free_pools(AppState &st)
{
    for (auto *p : st.pools)
        rte_mempool_free(p);
    st.pools.clear();
}
