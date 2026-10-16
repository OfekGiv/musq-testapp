#include "esp_hw.hpp"

#if ENABLE_ESP_HW

#include <doca_dev.h>
#include <doca_dpdk.h>
#include <doca_error.h>
#include <doca_flow.h>
#include <doca_flow_crypto.h>

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ethdev.h>

#include <arpa/inet.h>

#include <cstring>
#include <string>
#include <vector>

namespace {

struct EntryStatus {
    int processed = 0;
    bool failure = false;
};

struct doca_dev *g_dev = nullptr;
struct doca_flow_port *g_port = nullptr;
bool g_flow_up = false;
struct doca_flow_pipe *g_rx_pipe = nullptr;
struct doca_flow_pipe *g_tx_root = nullptr;
struct doca_flow_pipe *g_tx_encap = nullptr;
struct doca_flow_pipe_entry *g_encap_entry = nullptr;
uint32_t g_sa_id = 0;
/* RSS queue lists must stay valid until their entries are processed. */
std::vector<std::vector<uint16_t>> g_rss_queues;

/* Test key material: the same for every SA, nothing here needs to be secret. */
uint32_t g_key[4] = {0x00112233, 0x44556677, 0x8899aabb, 0xccddeeff};
constexpr uint32_t SALT = 0x12345678;

#define ESP_CHECK(call, what)                                                                  \
    do {                                                                                       \
        const doca_error_t r_ = (call);                                                        \
        if (r_ != DOCA_SUCCESS) {                                                              \
            APP_LOG_ERR("esp-encap hw: %s: %s", what, doca_error_get_descr(r_));               \
            return -1;                                                                         \
        }                                                                                      \
    } while (0)

void entry_cb(struct doca_flow_pipe_entry *, uint16_t, enum doca_flow_entry_status status, enum doca_flow_entry_op,
              void *user_ctx)
{
    auto *s = static_cast<EntryStatus *>(user_ctx);
    if (s == nullptr)
        return;
    if (status != DOCA_FLOW_ENTRY_STATUS_SUCCESS)
        s->failure = true;
    s->processed++;
}

/* Push the queued entries to hardware and wait until all `n` are done. */
int process_entries(EntryStatus &s, int n, const char *what)
{
    const uint64_t deadline = rte_rdtsc() + 5 * rte_get_tsc_hz();
    while (s.processed < n && !s.failure && rte_rdtsc() < deadline) {
        const doca_error_t r = doca_flow_entries_process(g_port, 0, 10000, static_cast<uint32_t>(n));
        if (r != DOCA_SUCCESS) {
            APP_LOG_ERR("esp-encap hw: %s: entries_process: %s", what, doca_error_get_descr(r));
            return -1;
        }
    }
    if (s.failure || s.processed != n) {
        APP_LOG_ERR("esp-encap hw: %s: %d of %d rules accepted by the NIC%s", what, s.processed, n,
                    s.failure ? " (one was rejected)" : " (timeout)");
        return -1;
    }
    return 0;
}

int open_dev(const char *pci)
{
    struct doca_devinfo **list = nullptr;
    uint32_t n = 0;
    ESP_CHECK(doca_devinfo_create_list(&list, &n), "list DOCA devices");
    doca_error_t r = DOCA_ERROR_NOT_FOUND;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t eq = 0;
        if (doca_devinfo_is_equal_pci_addr(list[i], pci, &eq) == DOCA_SUCCESS && eq) {
            r = doca_dev_open(list[i], &g_dev);
            break;
        }
    }
    doca_devinfo_destroy_list(list);
    ESP_CHECK(r, "open the DOCA device");
    return 0;
}

int start_flow(const AppState &st)
{
    struct doca_flow_cfg *fc = nullptr;
    ESP_CHECK(doca_flow_cfg_create(&fc), "flow cfg");
    doca_error_t r = doca_flow_cfg_set_pipe_queues(fc, 1);
    if (r == DOCA_SUCCESS)
        r = doca_flow_cfg_set_mode_args(fc, "vnf,hws");
    if (r == DOCA_SUCCESS)
        r = doca_flow_cfg_set_queue_depth(fc, 128);
    if (r == DOCA_SUCCESS)
        r = doca_flow_cfg_set_cb_entry_process(fc, entry_cb);
    if (r == DOCA_SUCCESS)
        r = doca_flow_cfg_set_resource_mode(fc, DOCA_FLOW_RESOURCE_MODE_PORT);
    if (r == DOCA_SUCCESS)
        r = doca_flow_init(fc);
    doca_flow_cfg_destroy(fc);
    ESP_CHECK(r, "doca_flow_init (vnf,hws)");
    g_flow_up = true;

    struct doca_flow_port_cfg *pc = nullptr;
    ESP_CHECK(doca_flow_port_cfg_create(&pc), "port cfg");
    r = doca_flow_port_cfg_set_port_id(pc, st.port_id);
    if (r == DOCA_SUCCESS)
        r = doca_flow_port_cfg_set_dev(pc, g_dev);
    if (r == DOCA_SUCCESS)
        r = doca_flow_port_cfg_set_nr_resources(pc, DOCA_FLOW_RESOURCE_IPSEC_SA, 1);
    if (r == DOCA_SUCCESS) /* the counter of the egress rule */
        r = doca_flow_port_cfg_set_nr_resources(pc, DOCA_FLOW_RESOURCE_COUNTER, 16);
    if (r == DOCA_SUCCESS)
        r = doca_flow_port_cfg_set_actions_mem_size(pc, 1u << 20);
    if (r == DOCA_SUCCESS)
        r = doca_flow_port_start(pc, &g_port);
    doca_flow_port_cfg_destroy(pc);
    ESP_CHECK(r, "doca_flow_port_start");
    return 0;
}

/* Ingress: IPv4 destination -> the RX queue of the worker that owns it. */
int create_rx_pipe(const AppState &st, const std::vector<rte_be32_t> &ips, const std::vector<std::vector<uint16_t>> &owners)
{
    struct doca_flow_match match;
    struct doca_flow_fwd fwd, miss;
    memset(&match, 0, sizeof(match));
    memset(&fwd, 0, sizeof(fwd));
    memset(&miss, 0, sizeof(miss));
    match.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
    match.outer.ip4.dst_ip = 0xffffffff;
    fwd.type = DOCA_FLOW_FWD_CHANGEABLE;
    miss.type = DOCA_FLOW_FWD_DROP;

    struct doca_flow_pipe_cfg *cfg = nullptr;
    ESP_CHECK(doca_flow_pipe_cfg_create(&cfg, g_port), "rx pipe cfg");
    doca_error_t r = doca_flow_pipe_cfg_set_name(cfg, "REQ_RX_STEER");
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_is_root(cfg, true);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_nr_entries(cfg, static_cast<uint32_t>(ips.size()));
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_match(cfg, &match, nullptr);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_create(cfg, &fwd, &miss, &g_rx_pipe);
    doca_flow_pipe_cfg_destroy(cfg);
    ESP_CHECK(r, "create the RX steering pipe");

    EntryStatus status;
    g_rss_queues = owners;
    for (size_t i = 0; i < ips.size(); i++) {
        struct doca_flow_match em;
        struct doca_flow_fwd ef;
        memset(&em, 0, sizeof(em));
        memset(&ef, 0, sizeof(ef));
        em.outer.l3_type = DOCA_FLOW_L3_TYPE_IP4;
        em.outer.ip4.dst_ip = ips[i];
        ef.type = DOCA_FLOW_FWD_RSS;
        ef.rss_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
        ef.rss.queues_array = g_rss_queues[i].data();
        ef.rss.nr_queues = static_cast<int>(g_rss_queues[i].size());
        ef.rss.outer_flags = DOCA_FLOW_RSS_IPV4;
        ESP_CHECK(doca_flow_pipe_basic_add_entry(0, g_rx_pipe, &em, 0, nullptr, nullptr, &ef,
                                                 DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &status, nullptr),
                  "add an RX steering rule");
    }
    (void)st;
    return process_entries(status, static_cast<int>(ips.size()), "RX steering");
}

int configure_sa(uint32_t id)
{
    struct doca_flow_shared_resource_cfg cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.ipsec_sa_cfg.key_cfg.key_type = DOCA_FLOW_CRYPTO_KEY_128;
    cfg.ipsec_sa_cfg.key_cfg.key = g_key;
    cfg.ipsec_sa_cfg.salt = SALT;
    cfg.ipsec_sa_cfg.implicit_iv = 0;
    cfg.ipsec_sa_cfg.icv_len = DOCA_FLOW_CRYPTO_ICV_LENGTH_16;
    cfg.ipsec_sa_cfg.sn_offload_type = DOCA_FLOW_CRYPTO_SN_OFFLOAD_INC;
    cfg.ipsec_sa_cfg.esn_en = false;
    cfg.ipsec_sa_cfg.sn_initial = 0;
    ESP_CHECK(doca_flow_port_shared_resource_set_cfg(g_port, DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, id, &cfg),
              "configure an IPsec SA");
    return 0;
}

/* Egress: every IPv4 packet any lcore sends is ESP-encapsulated and encrypted
 * with the one SA, so the NIC's sequence number runs over all of them together,
 * in the order they leave. */
int create_tx_pipes(const AppState &st)
{
    /* The encapsulating pipe. SA id and header bytes are given with its entry. */
    struct doca_flow_match match;
    struct doca_flow_actions actions, *actions_arr[1] = {&actions};
    struct doca_flow_monitor monitor;
    struct doca_flow_fwd fwd, miss;
    memset(&match, 0, sizeof(match));
    memset(&actions, 0, sizeof(actions));
    memset(&monitor, 0, sizeof(monitor));
    memset(&fwd, 0, sizeof(fwd));
    memset(&miss, 0, sizeof(miss));
    match.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    actions.has_crypto_encap = true;
    actions.crypto_encap.action_type = DOCA_FLOW_CRYPTO_REFORMAT_ENCAP;
    actions.crypto_encap.net_type = DOCA_FLOW_CRYPTO_HEADER_ESP_OVER_IPV4;
    actions.crypto_encap.icv_size = ESP_HW_ICV_LEN;
    actions.crypto_encap.data_size = ESP_HW_HDR_LEN;
    memset(actions.crypto_encap.encap_data, 0xff, ESP_HW_HDR_LEN);
    actions.crypto.action_type = DOCA_FLOW_CRYPTO_ACTION_ENCRYPT;
    actions.crypto.resource_type = DOCA_FLOW_CRYPTO_RESOURCE_IPSEC_SA;
    actions.crypto.ipsec_sa.sn_en = true;
    actions.crypto.crypto_id = UINT32_MAX;
    monitor.counter_type = DOCA_FLOW_RESOURCE_TYPE_NON_SHARED;
    fwd.type = DOCA_FLOW_FWD_PORT;
    fwd.port_id = st.port_id;
    miss.type = DOCA_FLOW_FWD_DROP;

    struct doca_flow_pipe_cfg *cfg = nullptr;
    ESP_CHECK(doca_flow_pipe_cfg_create(&cfg, g_port), "encap pipe cfg");
    doca_error_t r = doca_flow_pipe_cfg_set_name(cfg, "REQ_TX_ESP_ENCAP");
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_match(cfg, &match, nullptr);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_actions(cfg, actions_arr, nullptr, nullptr, 1);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_monitor(cfg, &monitor);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_create(cfg, &fwd, &miss, &g_tx_encap);
    doca_flow_pipe_cfg_destroy(cfg);
    ESP_CHECK(r, "create the ESP encapsulation pipe (IPsec offload on egress)");

    /* The egress root: every IPv4 packet the lcores send goes to it. */
    struct doca_flow_match rmatch;
    struct doca_flow_fwd rfwd, rmiss;
    memset(&rmatch, 0, sizeof(rmatch));
    memset(&rfwd, 0, sizeof(rfwd));
    memset(&rmiss, 0, sizeof(rmiss));
    rmatch.parser_meta.outer_l3_type = DOCA_FLOW_L3_META_IPV4;
    rfwd.type = DOCA_FLOW_FWD_PIPE;
    rfwd.next_pipe = g_tx_encap;
    rmiss.type = DOCA_FLOW_FWD_DROP;
    ESP_CHECK(doca_flow_pipe_cfg_create(&cfg, g_port), "egress root cfg");
    r = doca_flow_pipe_cfg_set_name(cfg, "REQ_TX_ROOT");
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_type(cfg, DOCA_FLOW_PIPE_BASIC);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_is_root(cfg, true);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_domain(cfg, DOCA_FLOW_PIPE_DOMAIN_SECURE_EGRESS);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_nr_entries(cfg, 1);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_cfg_set_match(cfg, &rmatch, nullptr);
    if (r == DOCA_SUCCESS)
        r = doca_flow_pipe_create(cfg, &rfwd, &rmiss, &g_tx_root);
    doca_flow_pipe_cfg_destroy(cfg);
    ESP_CHECK(r, "create the egress root pipe");

    EntryStatus status;
    ESP_CHECK(doca_flow_pipe_basic_add_entry(0, g_tx_root, &rmatch, 0, nullptr, nullptr, nullptr,
                                             DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &status, nullptr),
              "add the egress root rule");

    ESP_CHECK(doca_flow_port_shared_resource_get(g_port, DOCA_FLOW_SHARED_RESOURCE_IPSEC_SA, &g_sa_id),
              "allocate the IPsec SA");
    if (configure_sa(g_sa_id) < 0)
        return -1;
    struct doca_flow_actions ea = actions;
    /* SPI, then SN and IV, which the NIC fills in. */
    memset(ea.crypto_encap.encap_data, 0, ESP_HW_HDR_LEN);
    const rte_be32_t spi = rte_cpu_to_be_32(ESP_HW_SPI);
    memcpy(ea.crypto_encap.encap_data, &spi, sizeof(spi));
    ea.crypto.crypto_id = g_sa_id;
    ESP_CHECK(doca_flow_pipe_basic_add_entry(0, g_tx_encap, &match, 0, &ea, nullptr, nullptr,
                                             DOCA_FLOW_ENTRY_FLAGS_NO_WAIT, &status, &g_encap_entry),
              "add the ESP encapsulation rule");
    return process_entries(status, 2, "ESP encapsulation");
}

} // namespace

bool esp_hw_available() { return true; }

int esp_hw_probe(const AppConfig &cfg)
{
    if (open_dev(cfg.pci.c_str()) < 0)
        return -1;
    std::string devargs = "dv_flow_en=2";
    if (!cfg.devargs.empty())
        devargs += "," + cfg.devargs;
    ESP_CHECK(doca_dpdk_port_probe(g_dev, devargs.c_str()), "probe the port through the DOCA DPDK bridge");
    if (rte_eth_dev_count_avail() != 1) {
        APP_LOG_ERR("esp-encap hw: expected exactly one ethdev after the DOCA probe, found %u", rte_eth_dev_count_avail());
        return -1;
    }
    APP_LOG_INFO("esp-encap hw: DOCA device %s probed (devargs '%s')", cfg.pci.c_str(), devargs.c_str());
    return 0;
}

int esp_hw_start(AppState &st, const rte_be32_t *src_ips)
{
    /* One RX rule per distinct source IP; the workers that share an IP have
     * their responses spread over their queues. */
    std::vector<rte_be32_t> ips;
    std::vector<std::vector<uint16_t>> owners;
    for (unsigned i = 0; i < st.nb_workers; i++) {
        size_t k = 0;
        while (k < ips.size() && ips[k] != src_ips[i])
            k++;
        if (k == ips.size()) {
            ips.push_back(src_ips[i]);
            owners.emplace_back();
        }
        owners[k].push_back(st.workers[i].queue_id);
    }
    if (start_flow(st) < 0)
        return -1;
    if (create_rx_pipe(st, ips, owners) < 0)
        return -1;
    if (create_tx_pipes(st) < 0)
        return -1;
    APP_LOG_INFO("esp-encap hw: all %u lcores -> one SA (id %u, SPI 0x%x, AES-GCM-128, ICV %u B): "
                 "one sequence number stream over all of them, counted by the NIC",
                 st.nb_workers, g_sa_id, ESP_HW_SPI, ESP_HW_ICV_LEN);
    for (size_t k = 0; k < ips.size(); k++) {
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &ips[k], ip, sizeof(ip));
        APP_LOG_INFO("esp-encap hw: responses to %s -> %zu RX queue(s) from %u", ip, owners[k].size(), owners[k][0]);
    }
    return 0;
}

uint64_t esp_hw_encap_pkts()
{
    struct doca_flow_resource_query q;
    memset(&q, 0, sizeof(q));
    if (g_encap_entry == nullptr || doca_flow_resource_query_entry(g_encap_entry, &q) != DOCA_SUCCESS)
        return 0;
    return q.counter.total_pkts;
}

void esp_hw_stop()
{
    if (g_port != nullptr) {
        APP_LOG_INFO("esp-encap hw: %lu packets encapsulated by the NIC", static_cast<unsigned long>(esp_hw_encap_pkts()));
        const doca_error_t r = doca_flow_port_stop(g_port);
        if (r != DOCA_SUCCESS)
            APP_LOG_WARN("esp-encap hw: doca_flow_port_stop: %s", doca_error_get_descr(r));
        g_port = nullptr;
    }
    g_encap_entry = nullptr;
    g_rx_pipe = g_tx_root = g_tx_encap = nullptr;
}

void esp_hw_cleanup()
{
    if (g_flow_up) {
        doca_flow_destroy();
        g_flow_up = false;
    }
    if (g_dev != nullptr) {
        doca_dev_close(g_dev);
        g_dev = nullptr;
    }
}

#else /* !ENABLE_ESP_HW */

bool esp_hw_available() { return false; }
int esp_hw_probe(const AppConfig &)
{
    APP_LOG_ERR("--esp-encap hw needs a build with -Desp_hw=enabled (system DOCA Flow)");
    return -1;
}
int esp_hw_start(AppState &, const rte_be32_t *) { return -1; }
uint64_t esp_hw_encap_pkts() { return 0; }
void esp_hw_stop() {}
void esp_hw_cleanup() {}

#endif
