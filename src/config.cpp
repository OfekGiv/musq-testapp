#include "config.hpp"

#include <cxxopts.hpp>
#include <fmt/format.h>

#include <arpa/inet.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

static bool parse_lcore_list(const std::string &s, std::vector<unsigned> &out, std::string &err)
{
    out.clear();
    std::stringstream ss(s);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (tok.empty())
            continue;
        const auto dash = tok.find('-');
        char *end = nullptr;
        if (dash == std::string::npos) {
            const unsigned long v = strtoul(tok.c_str(), &end, 10);
            if (end == tok.c_str() || *end != '\0' || v >= RTE_MAX_LCORE) {
                err = "bad lcore '" + tok + "'";
                return false;
            }
            out.push_back(static_cast<unsigned>(v));
        } else {
            const std::string a = tok.substr(0, dash), b = tok.substr(dash + 1);
            const unsigned long lo = strtoul(a.c_str(), &end, 10);
            if (end == a.c_str() || *end != '\0') { err = "bad lcore range '" + tok + "'"; return false; }
            const unsigned long hi = strtoul(b.c_str(), &end, 10);
            if (end == b.c_str() || *end != '\0' || hi < lo || hi >= RTE_MAX_LCORE) {
                err = "bad lcore range '" + tok + "'";
                return false;
            }
            for (unsigned long v = lo; v <= hi; v++)
                out.push_back(static_cast<unsigned>(v));
        }
    }
    std::sort(out.begin(), out.end());
    if (std::adjacent_find(out.begin(), out.end()) != out.end()) {
        err = "duplicate lcore in '" + s + "'";
        return false;
    }
    if (out.empty()) {
        err = "empty lcore list";
        return false;
    }
    return true;
}

static bool parse_u64(const std::string &s, uint64_t &v)
{
    char *end = nullptr;
    v = strtoull(s.c_str(), &end, 10);
    return end != s.c_str() && *end == '\0';
}

static bool parse_port_range(const std::string &s, uint16_t &lo, uint16_t &hi)
{
    uint64_t a, b;
    const auto dash = s.find('-');
    if (dash == std::string::npos) {
        if (!parse_u64(s, a) || a > 65535)
            return false;
        lo = hi = static_cast<uint16_t>(a);
        return true;
    }
    if (!parse_u64(s.substr(0, dash), a) || !parse_u64(s.substr(dash + 1), b) || a > 65535 || b > 65535 || b < a)
        return false;
    lo = static_cast<uint16_t>(a);
    hi = static_cast<uint16_t>(b);
    return true;
}

/* fixed:<ns> | bimodal:<lo_ns>/<hi_ns>/<pct_hi> | exp:<mean_ns> | header */
static bool parse_work_spec(const std::string &s, WorkSpec &w, std::string &err)
{
    w = WorkSpec{};
    if (s == "header") {
        w.kind = WorkKind::Header;
        return true;
    }
    const auto colon = s.find(':');
    const std::string kind = s.substr(0, colon);
    const std::string arg = colon == std::string::npos ? "" : s.substr(colon + 1);
    if (kind == "fixed") {
        w.kind = WorkKind::Fixed;
        if (!parse_u64(arg, w.lo_ns)) { err = "fixed:<ns> expected"; return false; }
        return true;
    }
    if (kind == "exp") {
        w.kind = WorkKind::Exp;
        if (!parse_u64(arg, w.lo_ns)) { err = "exp:<mean_ns> expected"; return false; }
        return true;
    }
    if (kind == "bimodal") {
        w.kind = WorkKind::Bimodal;
        std::stringstream ss(arg);
        std::string a, b, c;
        uint64_t pct;
        if (!std::getline(ss, a, '/') || !std::getline(ss, b, '/') || !std::getline(ss, c, '/') ||
            !parse_u64(a, w.lo_ns) || !parse_u64(b, w.hi_ns) || !parse_u64(c, pct) || pct > 100) {
            err = "bimodal:<lo_ns>/<hi_ns>/<pct_hi> expected (pct 0..100)";
            return false;
        }
        w.pct_hi = static_cast<uint32_t>(pct);
        return true;
    }
    err = "unknown work kind '" + kind + "'";
    return false;
}

std::string work_spec_to_string(const WorkSpec &w)
{
    switch (w.kind) {
    case WorkKind::Fixed: return fmt::format("fixed:{}", w.lo_ns);
    case WorkKind::Bimodal: return fmt::format("bimodal:{}/{}/{}", w.lo_ns, w.hi_ns, w.pct_hi);
    case WorkKind::Exp: return fmt::format("exp:{}", w.lo_ns);
    case WorkKind::Header: return "header";
    }
    return "?";
}

const char *doca_mode_name(DocaMode m)
{
    switch (m) {
    case DocaMode::None: return "none";
    case DocaMode::MetaRr: return "meta_rr";
    case DocaMode::EspRr: return "esp_rr";
    }
    return "?";
}
const char *tx_mode_name(TxMode m) { return m == TxMode::Immediate ? "immediate" : "accumulate"; }
const char *swap_mode_name(SwapMode m)
{
    switch (m) {
    case SwapMode::L2L3: return "l2l3";
    case SwapMode::L2: return "l2";
    case SwapMode::None: return "none";
    }
    return "?";
}
const char *role_name(Role r) { return r == Role::Responder ? "responder" : "requester"; }
const char *dist_order_name(DistOrder o) { return o == DistOrder::Seqn ? "seqn" : "rr"; }
const char *dist_deal_name(DistDeal d) { return d == DistDeal::Lb ? "lb" : "rr"; }
const char *tx_fill_name(TxFill f) { return f == TxFill::Prefill ? "prefill" : "copy"; }
const char *rx_steer_name(RxSteer s)
{
    switch (s) {
    case RxSteer::Flow: return "flow";
    case RxSteer::Rss: return "rss";
    case RxSteer::None: return "none";
    }
    return "?";
}
const char *xstats_mode_name(XstatsMode m)
{
    switch (m) {
    case XstatsMode::Filtered: return "filtered";
    case XstatsMode::All: return "all";
    case XstatsMode::Final: return "final";
    }
    return "?";
}

template <typename T>
static bool parse_enum(const std::string &s, const std::vector<std::pair<const char *, T>> &tbl, T &out)
{
    for (const auto &e : tbl)
        if (s == e.first) { out = e.second; return true; }
    return false;
}

int parse_cli(int argc, char **argv, AppConfig &cfg)
{
    cxxopts::Options opts("musq-testapp",
        "Receive-and-send-back DPDK test app for the mu_sq / DOCA Flow mlx5 feature");
    std::string lcores, doca_mode, tx_mode, work, swap, xstats, role, musq_group, esp_encap, masters, tx_masters;
    bool shared_pool = false, no_promisc = false, no_esp_sn_check = false;
    opts.add_options()
        ("pci", "PCI BDF of the data-path port", cxxopts::value<std::string>()->default_value(cfg.pci))
        ("devargs", "Extra mlx5 devargs (appended after mu_sq_log_grp_size)", cxxopts::value<std::string>()->default_value(""))
        ("musq-group", "mu_sq group size (lanes per group): auto (= TX queues / --musq-groups; 0 with --masters), 0 (off) or a power of 2 <= 128", cxxopts::value<std::string>()->default_value("auto"))
        ("musq-groups", "mu_sq groups on the port, each its own shared TX ring (power of 2 <= 64): group g = TX queues g*N..g*N+N-1 with N lanes (--musq-group), so --lcores is G x N lcores, one RX and one TX queue each. In a DOCA mode the NIC hashes a flow (outer IPv4 src+dst) to one group and round-robins inside it (esp_rr only). Needs the prom-dpdk PMD with multi-group mu_sq", cxxopts::value<unsigned>()->default_value("1"))
        ("hash-probe", "responder, DOCA mode with --musq-groups > 1: <src-ip>[+n]:<dst-ip>. Log at start which group each of the n flows src-ip, src-ip+1, .. -> dst-ip lands in (the requester sends from --src-ip + lcore index), e.g. 1.1.1.1+8:2.2.2.2", cxxopts::value<std::string>()->default_value(""))
        ("l,lcores", "Worker lcores, e.g. 2,4,6,8 or 2-8", cxxopts::value<std::string>())
        ("main-lcore", "Lcore running EAL main + stats (not a worker)", cxxopts::value<unsigned>()->default_value("0"))
        ("doca-mode", "none | meta_rr | esp_rr (DOCA Flow steering pipeline)", cxxopts::value<std::string>()->default_value("esp_rr"))
        ("doca-group", "DOCA round-robin steering group size (power of 2)", cxxopts::value<uint16_t>()->default_value("1"))
        ("rx-burst", "rte_eth_rx_burst size (<= tx-burst, except for a responder in accumulate mode, which splits a large RX burst into tx-burst sends)", cxxopts::value<uint16_t>()->default_value("32"))
        ("tx-burst", "Max packets per rte_eth_tx_burst call (power of 2; one mu_sq slot)", cxxopts::value<uint16_t>()->default_value("32"))
        ("tx-mode", "immediate | accumulate (buffer up to tx-burst, drain on timer)", cxxopts::value<std::string>()->default_value("accumulate"))
        ("tx-drain-us", "TX buffer drain interval in us (accumulate mode)", cxxopts::value<uint64_t>()->default_value("100"))
        ("work", "Mock work per packet: fixed:<ns> | bimodal:<lo_ns>/<hi_ns>/<pct_hi> | exp:<mean_ns> | header", cxxopts::value<std::string>()->default_value("fixed:0"))
        ("swap", "Header rewrite before send-back: l2l3 | l2 | none", cxxopts::value<std::string>()->default_value("l2l3"))
        ("rxd", "RX descriptors per queue", cxxopts::value<uint16_t>()->default_value("1024"))
        ("txd", "TX descriptors per queue", cxxopts::value<uint16_t>()->default_value("1024"))
        ("shared-pool", "One mbuf pool shared by all workers (default: one pool per worker lcore)", cxxopts::value<bool>(shared_pool))
        ("no-promisc", "Do not enable promiscuous mode", cxxopts::value<bool>(no_promisc))
        ("duration", "Run time in seconds (0 = until SIGINT/SIGTERM)", cxxopts::value<double>()->default_value("0"))
        ("warmup", "Seconds before measurement baselines are taken", cxxopts::value<double>()->default_value("0"))
        ("stats-interval", "Seconds between stats reports", cxxopts::value<double>()->default_value("1"))
        ("stats-csv", "Append long-format CSV rows to this file", cxxopts::value<std::string>()->default_value(""))
        ("xstats", "filtered | all | final (xstats printing)", cxxopts::value<std::string>()->default_value("filtered"))
        ("role", "responder | requester (requester: phase 2, not implemented)", cxxopts::value<std::string>()->default_value("responder"))
        ("file-prefix", "EAL --file-prefix", cxxopts::value<std::string>()->default_value("musq"))
        ("log-level", "EAL --log-level (7 = INFO, needed for the \"entering main loop\" ready line)", cxxopts::value<int>()->default_value("7"))
        ("eal-args", "Extra EAL arguments, space separated (e.g. \"--trace=pmd.net.mlx5.tx.lcore\")", cxxopts::value<std::string>()->default_value(""))
        ("rps", "requester: requests per second PER requester lcore", cxxopts::value<uint64_t>()->default_value("1000000"))
        ("service-time", "requester: per-request service time asked of the responder (ns spec like --work; sent as us): fixed:<ns> | bimodal:<lo>/<hi>/<pct_hi> | exp:<mean>", cxxopts::value<std::string>()->default_value("fixed:0"))
        ("traffic", "requester: udp | esp (esp = IPv4 proto 50 with incrementing SN, for the esp_rr DUT)", cxxopts::value<std::string>()->default_value("esp"))
        ("frame-size", "requester: frame size on the wire incl. FCS", cxxopts::value<uint16_t>()->default_value("92"))
        ("dst-mac", "requester: destination MAC", cxxopts::value<std::string>()->default_value("f4:20:4d:24:44:86"))
        ("src-mac", "requester: source MAC (default: port MAC)", cxxopts::value<std::string>()->default_value(""))
        ("src-ip", "requester: source IPv4", cxxopts::value<std::string>()->default_value("1.1.1.1"))
        ("dst-ip", "requester: destination IPv4", cxxopts::value<std::string>()->default_value("2.2.2.2"))
        ("sport", "requester/udp: source port or range a-b, cycled per packet", cxxopts::value<std::string>()->default_value("1025-1040"))
        ("dport", "requester/udp: destination port or range a-b", cxxopts::value<std::string>()->default_value("12345"))
        ("tracker-size", "requester: outstanding-request slots per lcore (power of 2)", cxxopts::value<uint32_t>()->default_value("1048576"))
        ("rx-steer", "requester: flow (each lcore sends from src-ip+index, an rte_flow rule returns its responses to its own queue) | rss (per-lcore src IP, RSS spreads responses) | none (one src IP; ESP responses all land on one queue)", cxxopts::value<std::string>()->default_value("flow"))
        ("tx-fill", "requester: prefill (dedicated TX mbuf pool per lcore, the frame template is written into every mbuf once and only the per-request fields are rewritten) | copy (rte_memcpy the whole template into each mbuf, korba behaviour)", cxxopts::value<std::string>()->default_value("prefill"))
        ("esp-encap", "requester, --traffic esp: sw (the lcores write the ESP header and sequence number; nothing is encrypted) | hw (the lcores send plain UDP frames and the NIC encapsulates them as ESP transport mode with AES-GCM: SPI, one sequence number stream counted by the NIC over all lcores together, IV, trailer and ICV; IPsec packet offload through DOCA Flow, needs the build-esphw binary that scripts/run.sh selects; responses come back encrypted and are only counted)", cxxopts::value<std::string>()->default_value("sw"))
        ("esp-sa", "requester, --esp-encap hw: lcore (one IPsec SA per lcore: its own SPI, --esp-spi + lcore index, and its own sequence-number counter in the NIC; needs distinct source IPs) | port (one SA and one SN counter over all lcores together, the original mode; not against a responder with --musq-groups > 1, whose hash would split that one SN stream)", cxxopts::value<std::string>()->default_value("lcore"))
        ("esp-spi", "requester, --traffic esp: SPI base (decimal or 0x..); lcore i writes/gets base + i with --esp-sa lcore (the sw encap too), base alone with --esp-sa port", cxxopts::value<std::string>()->default_value("0x1000"))
        ("src-ips", "requester: explicit source IPv4 per lcore, comma separated in lcore order (as many as --lcores), instead of --src-ip + index; e.g. chosen from the responder's --hash-probe log so that each flow lands in its own mu_sq group", cxxopts::value<std::string>()->default_value(""))
        ("no-esp-sn-check", "requester, --traffic esp: do not check that each lcore's responses come back with increasing ESP sequence numbers (rows \"ESP SN reorders\" etc.)", cxxopts::value<bool>(no_esp_sn_check))
        ("tx-file", "requester: stream correctness test; each lcore sends this file once as its own flow, one chunk per request ([8 B chunk index][file bytes], ESP SN = chunk index), then stops sending", cxxopts::value<std::string>()->default_value(""))
        ("rx-record", "requester: record each lcore's responses in arrival order to <prefix>.w<worker>.{data,idx,json} at exit (needs --tx-file; compare with scripts/stream_compare.py)", cxxopts::value<std::string>()->default_value(""))
        ("pkts-per-wqe", "Packets per TX WQE: 32 (default; mu_sq 32-packet slots / eMPW) or 1 (one packet per WQE: mu_sq 1-WQEBB slots, needs the 1-packet PMD build that scripts/run.sh selects; without mu_sq adds txq_mpw_en=0)", cxxopts::value<unsigned>()->default_value("32"))
        ("tx-lanes", "responder, one worker lcore: TX queues (mu_sq lanes) that lcore posts to, dealing the packets to them in turn (packet i to lane i % n), each lane with its own buffer and rte_eth_tx_burst calls: one core doing the TX work of n. A power of 2 <= 8; --musq-group auto then is n. (One lcore with ONE TX queue that drives every lane of a group itself is --tx-lanes 1 --musq-group <lanes>.)", cxxopts::value<unsigned>()->default_value("1"))
        ("rx-queues", "responder, --doca-mode none: RX queues per worker lcore, polled in turn (one rte_eth_rx_burst per queue per loop pass); RSS spreads the traffic over all workers' queues. TX is unchanged: one TX queue per worker, or --tx-lanes. At most 8", cxxopts::value<unsigned>()->default_value("1"))
        ("masters", "responder: software distributor. These lcores receive, deal the packets to the worker lcores (--lcores, split evenly among the masters in lcore order) over rings, take them back in arrival order and send them on their own TX queue; the workers only process packets (--work, --swap). Master m owns RX queue(s) m*rx-queues.. and TX queue m. The software alternative to mu_sq (no mu_sq; not the EAL main lcore). See README, \"Software distributor\"", cxxopts::value<std::string>()->default_value(""))
        ("tx-masters", "--masters: one lcore per master (paired in lcore order) that takes over the collecting and sending half of that master, a two-stage pipeline: the master then only receives and deals, the TX master collects the workers' returns in arrival order and sends them on the master's TX queue. Not in --masters, --lcores or the main lcore", cxxopts::value<std::string>()->default_value(""))
        ("dist-order", "--masters: how a master restores the arrival order: rr (deal and collect round robin; the order is implicit, no sequence numbers) | seqn (sequence numbers kept by the master and a reorder buffer)", cxxopts::value<std::string>()->default_value("rr"))
        ("dist-deal", "--masters: which worker gets the next chunk: rr (round robin) | lb (the one with the fewest packets in flight; needs --dist-order seqn)", cxxopts::value<std::string>()->default_value("rr"))
        ("dist-chunk", "--masters: consecutive packets dealt to one worker before moving to the next (1 = per-packet round robin, like 1-packet mu_sq slots; 32 = whole bursts, far fewer ring operations per packet; the order is kept either way)", cxxopts::value<unsigned>()->default_value("32"))
        ("dist-self", "--masters: the master processes up to this many packets per deal cycle itself (one more slot in the round robin, after the workers) whenever a worker has --dist-self-backlog or more packets outstanding: the workers are the limit and the master has cycles to spare. 0 = off", cxxopts::value<unsigned>()->default_value("0"))
        ("dist-self-backlog", "--dist-self: tokens waiting in some worker's input ring from which the master takes its share", cxxopts::value<unsigned>()->default_value("128"))
        ("dist-self-idle", "--dist-self: the master takes a share only when at least this percent of its recent loop passes (windows of 1024) were idle, i.e. it has the cycles; 0 = always when a worker is behind", cxxopts::value<unsigned>()->default_value("5"))
        ("dist-ring", "--masters: entries of each master <-> worker ring (power of 2, >= 4 x rx-burst); a worker has at most ring - rx-burst packets in flight, beyond that the master stops receiving", cxxopts::value<unsigned>()->default_value("2048"))
        ("db-wqes", "responder with mu_sq: WQEs (mu_sq slots) a lane posts per doorbell. The PMD then rings the doorbell every that many WQEs, wherever a burst ends, instead of once per send pass (mlx5 devarg mu_sq_db_wqes; at most half the slots a lane has in the ring). A lane that posts nothing for --tx-drain-us rings what is held back. 0 = the PMD's own rule", cxxopts::value<unsigned>()->default_value("0"))
        ("lane-epoch-us", "responder: record each lane's cumulative RX/TX packets, busy/idle CPU cycles and WQ position (wqe_ci/pi/free, slots) once per epoch of this many us on a common TSC grid, dumped as CSV at exit (0 = off; the WQ columns need the prom-dpdk mlx5 PMD, DOCA build)", cxxopts::value<uint64_t>()->default_value("0"))
        ("lane-epoch-out", "CSV file for --lane-epoch-us", cxxopts::value<std::string>()->default_value("lane_epoch.csv"))
        ("lane-epoch-max", "Max records per lane for --lane-epoch-us (80 B each)", cxxopts::value<uint32_t>()->default_value("120000"))
        ("dry-run", "Print the resolved config and EAL argv, then exit")
        ("h,help", "Show help");

    cxxopts::ParseResult r;
    try {
        r = opts.parse(argc, argv);
    } catch (const std::exception &e) {
        fprintf(stderr, "error: %s\n", e.what());
        return -1;
    }
    if (r.count("help")) {
        printf("%s\n", opts.help().c_str());
        return 1;
    }

    std::string err;
    cfg.pci = r["pci"].as<std::string>();
    cfg.devargs = r["devargs"].as<std::string>();
    musq_group = r["musq-group"].as<std::string>();
    if (musq_group == "auto") {
        cfg.musq_group = -1;
    } else {
        uint64_t v;
        if (!parse_u64(musq_group, v) || v > MUSQ_MAX_GROUP) {
            fprintf(stderr, "error: --musq-group must be auto, 0 or a power of 2 <= %u\n", MUSQ_MAX_GROUP);
            return -1;
        }
        cfg.musq_group = static_cast<int>(v);
    }
    cfg.musq_groups = r["musq-groups"].as<unsigned>();
    {
        /* <src-ip>[+n]:<dst-ip> */
        const std::string hp = r["hash-probe"].as<std::string>();
        if (!hp.empty()) {
            const size_t colon = hp.find(':');
            if (colon == std::string::npos || colon == 0 || colon + 1 >= hp.size()) {
                fprintf(stderr, "error: --hash-probe must be <src-ip>[+n]:<dst-ip>\n");
                return -1;
            }
            std::string src = hp.substr(0, colon);
            cfg.hash_probe_dst = hp.substr(colon + 1);
            cfg.hash_probe_n = 1;
            const size_t plus = src.find('+');
            if (plus != std::string::npos) {
                uint64_t n;
                if (!parse_u64(src.substr(plus + 1), n) || n == 0 || n > 1024) {
                    fprintf(stderr, "error: --hash-probe: +n must be in 1..1024\n");
                    return -1;
                }
                cfg.hash_probe_n = static_cast<unsigned>(n);
                src.resize(plus);
            }
            cfg.hash_probe_src = src;
        }
    }
    if (!r.count("lcores")) {
        fprintf(stderr, "error: --lcores is required\n%s\n", opts.help().c_str());
        return -1;
    }
    if (!parse_lcore_list(r["lcores"].as<std::string>(), cfg.lcores, err)) {
        fprintf(stderr, "error: --lcores: %s\n", err.c_str());
        return -1;
    }
    cfg.main_lcore = r["main-lcore"].as<unsigned>();
    if (!parse_enum<DocaMode>(r["doca-mode"].as<std::string>(),
            {{"none", DocaMode::None}, {"meta_rr", DocaMode::MetaRr}, {"esp_rr", DocaMode::EspRr}}, cfg.doca_mode)) {
        fprintf(stderr, "error: --doca-mode must be none|meta_rr|esp_rr\n");
        return -1;
    }
    cfg.doca_group = r["doca-group"].as<uint16_t>();
    cfg.rx_burst = r["rx-burst"].as<uint16_t>();
    cfg.tx_burst = r["tx-burst"].as<uint16_t>();
    if (!parse_enum<TxMode>(r["tx-mode"].as<std::string>(),
            {{"immediate", TxMode::Immediate}, {"accumulate", TxMode::Accumulate}}, cfg.tx_mode)) {
        fprintf(stderr, "error: --tx-mode must be immediate|accumulate\n");
        return -1;
    }
    cfg.tx_drain_us = r["tx-drain-us"].as<uint64_t>();
    if (!parse_work_spec(r["work"].as<std::string>(), cfg.work, err)) {
        fprintf(stderr, "error: --work: %s\n", err.c_str());
        return -1;
    }
    if (!parse_enum<SwapMode>(r["swap"].as<std::string>(),
            {{"l2l3", SwapMode::L2L3}, {"l2", SwapMode::L2}, {"none", SwapMode::None}}, cfg.swap)) {
        fprintf(stderr, "error: --swap must be l2l3|l2|none\n");
        return -1;
    }
    cfg.rxd = r["rxd"].as<uint16_t>();
    cfg.txd = r["txd"].as<uint16_t>();
    cfg.per_lcore_pool = !shared_pool;
    cfg.promisc = !no_promisc;
    cfg.duration_s = r["duration"].as<double>();
    cfg.warmup_s = r["warmup"].as<double>();
    cfg.stats_interval_s = r["stats-interval"].as<double>();
    cfg.stats_csv = r["stats-csv"].as<std::string>();
    if (!parse_enum<XstatsMode>(r["xstats"].as<std::string>(),
            {{"filtered", XstatsMode::Filtered}, {"all", XstatsMode::All}, {"final", XstatsMode::Final}}, cfg.xstats)) {
        fprintf(stderr, "error: --xstats must be filtered|all|final\n");
        return -1;
    }
    if (!parse_enum<Role>(r["role"].as<std::string>(),
            {{"responder", Role::Responder}, {"requester", Role::Requester}}, cfg.role)) {
        fprintf(stderr, "error: --role must be responder|requester\n");
        return -1;
    }
    cfg.file_prefix = r["file-prefix"].as<std::string>();
    cfg.log_level = r["log-level"].as<int>();
    cfg.eal_args = r["eal-args"].as<std::string>();
    cfg.dry_run = r.count("dry-run") > 0;

    cfg.rps = r["rps"].as<uint64_t>();
    if (!parse_work_spec(r["service-time"].as<std::string>(), cfg.service_time, err) ||
        cfg.service_time.kind == WorkKind::Header) {
        fprintf(stderr, "error: --service-time: %s\n", err.empty() ? "header is not a service-time spec" : err.c_str());
        return -1;
    }
    if (!parse_enum<TrafficKind>(r["traffic"].as<std::string>(),
            {{"udp", TrafficKind::Udp}, {"esp", TrafficKind::Esp}}, cfg.traffic)) {
        fprintf(stderr, "error: --traffic must be udp|esp\n");
        return -1;
    }
    cfg.frame_size = r["frame-size"].as<uint16_t>();
    cfg.dst_mac = r["dst-mac"].as<std::string>();
    cfg.src_mac = r["src-mac"].as<std::string>();
    cfg.src_ip = r["src-ip"].as<std::string>();
    cfg.dst_ip = r["dst-ip"].as<std::string>();
    if (!parse_port_range(r["sport"].as<std::string>(), cfg.sport_lo, cfg.sport_hi) ||
        !parse_port_range(r["dport"].as<std::string>(), cfg.dport_lo, cfg.dport_hi)) {
        fprintf(stderr, "error: --sport/--dport must be <port> or <lo>-<hi>\n");
        return -1;
    }
    cfg.tracker_size = r["tracker-size"].as<uint32_t>();
    cfg.pkts_per_wqe = r["pkts-per-wqe"].as<unsigned>();
    cfg.tx_lanes = r["tx-lanes"].as<unsigned>();
    cfg.rx_queues = r["rx-queues"].as<unsigned>();
    cfg.db_wqes = r["db-wqes"].as<unsigned>();
    masters = r["masters"].as<std::string>();
    if (!masters.empty() && !parse_lcore_list(masters, cfg.masters, err)) {
        fprintf(stderr, "error: --masters: %s\n", err.c_str());
        return -1;
    }
    tx_masters = r["tx-masters"].as<std::string>();
    if (!tx_masters.empty() && !parse_lcore_list(tx_masters, cfg.tx_masters, err)) {
        fprintf(stderr, "error: --tx-masters: %s\n", err.c_str());
        return -1;
    }
    if (!parse_enum<DistOrder>(r["dist-order"].as<std::string>(),
            {{"rr", DistOrder::Rr}, {"seqn", DistOrder::Seqn}}, cfg.dist_order)) {
        fprintf(stderr, "error: --dist-order must be rr|seqn\n");
        return -1;
    }
    if (!parse_enum<DistDeal>(r["dist-deal"].as<std::string>(),
            {{"rr", DistDeal::Rr}, {"lb", DistDeal::Lb}}, cfg.dist_deal)) {
        fprintf(stderr, "error: --dist-deal must be rr|lb\n");
        return -1;
    }
    cfg.dist_chunk = r["dist-chunk"].as<unsigned>();
    cfg.dist_ring = r["dist-ring"].as<unsigned>();
    cfg.dist_self = r["dist-self"].as<unsigned>();
    cfg.dist_self_backlog = r["dist-self-backlog"].as<unsigned>();
    cfg.dist_self_idle = r["dist-self-idle"].as<unsigned>();
    cfg.lane_epoch_us = r["lane-epoch-us"].as<uint64_t>();
    cfg.lane_epoch_out = r["lane-epoch-out"].as<std::string>();
    cfg.lane_epoch_max = r["lane-epoch-max"].as<uint32_t>();
    if (!parse_enum<RxSteer>(r["rx-steer"].as<std::string>(),
            {{"flow", RxSteer::Flow}, {"rss", RxSteer::Rss}, {"none", RxSteer::None}}, cfg.rx_steer)) {
        fprintf(stderr, "error: --rx-steer must be flow|rss|none\n");
        return -1;
    }
    esp_encap = r["esp-encap"].as<std::string>();
    if (esp_encap != "sw" && esp_encap != "hw") {
        fprintf(stderr, "error: --esp-encap must be sw|hw\n");
        return -1;
    }
    cfg.esp_hw = esp_encap == "hw";
    {
        const std::string sa = r["esp-sa"].as<std::string>();
        if (sa != "lcore" && sa != "port") {
            fprintf(stderr, "error: --esp-sa must be lcore|port\n");
            return -1;
        }
        cfg.esp_sa_per_lcore = sa == "lcore";
        const std::string spi = r["esp-spi"].as<std::string>();
        char *end = nullptr;
        const unsigned long v = strtoul(spi.c_str(), &end, 0);
        if (spi.empty() || end == nullptr || *end != '\0' || v > 0xffffffffUL) {
            fprintf(stderr, "error: --esp-spi must be a 32-bit number (decimal or 0x..)\n");
            return -1;
        }
        cfg.esp_spi = static_cast<uint32_t>(v);
        cfg.esp_sn_check = !no_esp_sn_check;
        std::stringstream ips(r["src-ips"].as<std::string>());
        std::string ip;
        while (std::getline(ips, ip, ','))
            if (!ip.empty())
                cfg.src_ips.push_back(ip);
    }
    cfg.tx_file = r["tx-file"].as<std::string>();
    cfg.rx_record = r["rx-record"].as<std::string>();
    if (!parse_enum<TxFill>(r["tx-fill"].as<std::string>(),
            {{"prefill", TxFill::Prefill}, {"copy", TxFill::Copy}}, cfg.tx_fill)) {
        fprintf(stderr, "error: --tx-fill must be prefill|copy\n");
        return -1;
    }
    return 0;
}

bool validate_config(const AppConfig &cfg)
{
    bool ok = true;
    auto fail = [&](const std::string &m) { fprintf(stderr, "error: %s\n", m.c_str()); ok = false; };
    auto warn = [&](const std::string &m) { fprintf(stderr, "warning: %s\n", m.c_str()); };

    const unsigned nw = cfg.nb_workers();
    if (nw == 0)
        fail("at least one worker lcore is required");
    if (std::find(cfg.lcores.begin(), cfg.lcores.end(), cfg.main_lcore) != cfg.lcores.end())
        fail(fmt::format("--main-lcore {} must not be in --lcores", cfg.main_lcore));
    if (cfg.main_lcore >= RTE_MAX_LCORE)
        fail("--main-lcore out of range");

    if (cfg.doca_enabled()) {
        if (!is_pow2(nw))
            fail(fmt::format("DOCA mode needs a power-of-2 number of worker lcores (got {})", nw));
        if (!is_pow2(cfg.doca_group))
            fail(fmt::format("--doca-group {} must be a power of 2", cfg.doca_group));
#if !ENABLE_DOCA
        fail("built without DOCA support; use --doca-mode none");
#endif
    } else {
#if ENABLE_DOCA
        /* Merely loading the DOCA libraries next to the mlx5 PMD leaves the
         * RX queues without steering in plain mode (packets reach the vport,
         * never a queue -- verified 2026-09-25 against testpmd and a no-DOCA
         * build of this app). Plain RSS therefore needs the no-DOCA binary. */
        fail("--doca-mode none needs a build without the DOCA libraries: "
             "meson setup build-nodoca -Denable_doca=disabled -Dpkg_config_path=<...> && "
             "ninja -C build-nodoca (scripts/run.sh picks build-nodoca automatically)");
#endif
    }
    if (!is_pow2(cfg.tx_lanes) || cfg.tx_lanes > MAX_TX_LANES) {
        fail(fmt::format("--tx-lanes {} must be a power of 2 <= {}", cfg.tx_lanes, MAX_TX_LANES));
        return false;
    }
    if (cfg.tx_lanes > 1 && (nw != 1 || cfg.role != Role::Responder))
        fail("--tx-lanes > 1 needs the responder role and exactly one worker lcore");
    if (cfg.rx_queues == 0 || cfg.rx_queues > MAX_RX_QUEUES)
        fail(fmt::format("--rx-queues {} must be in 1..{}", cfg.rx_queues, MAX_RX_QUEUES));
    if (cfg.rx_queues > 1 && (cfg.role != Role::Responder || cfg.doca_enabled()))
        fail("--rx-queues > 1 needs the responder role and --doca-mode none "
             "(the DOCA pipelines and the requester's flow rules steer to one queue per worker)");
    if (cfg.dist()) {
        const unsigned nm = cfg.nb_masters(), wpm = cfg.workers_per_master();
        if (cfg.role != Role::Responder)
            fail("--masters is a responder option");
        if (std::find(cfg.masters.begin(), cfg.masters.end(), cfg.main_lcore) != cfg.masters.end())
            fail(fmt::format("--main-lcore {} must not be in --masters", cfg.main_lcore));
        for (unsigned m : cfg.masters)
            if (std::find(cfg.lcores.begin(), cfg.lcores.end(), m) != cfg.lcores.end())
                fail(fmt::format("lcore {} is in both --masters and --lcores", m));
        if (nw % nm != 0)
            fail(fmt::format("{} worker lcores do not split evenly among {} masters", nw, nm));
        if (wpm == 0 || wpm > MAX_DIST_WORKERS)
            fail(fmt::format("each master needs 1..{} workers (got {})", MAX_DIST_WORKERS, wpm));
        if (cfg.tx_lanes > 1)
            fail("--tx-lanes is not supported with --masters");
        if (cfg.musq_group > 0)
            fail("--masters is the software alternative to mu_sq: use --musq-group 0 (or auto)");
        if (cfg.doca_enabled() && (nm > 1 || cfg.rx_queues > 1))
            fail("a DOCA pipeline steers one flow round robin over all RX queues, which would reorder it before "
                 "the masters: --masters with a DOCA mode needs one master and --rx-queues 1 "
                 "(several masters: --doca-mode none, RSS keeps a flow on one queue)");
        if (cfg.dist_split()) {
            if (cfg.tx_masters.size() != nm)
                fail(fmt::format("--tx-masters needs one lcore per master ({} masters, {} TX masters)", nm,
                                 cfg.tx_masters.size()));
            for (unsigned t : cfg.tx_masters) {
                if (t == cfg.main_lcore)
                    fail(fmt::format("--main-lcore {} must not be in --tx-masters", t));
                if (std::find(cfg.masters.begin(), cfg.masters.end(), t) != cfg.masters.end())
                    fail(fmt::format("lcore {} is in both --masters and --tx-masters", t));
                if (std::find(cfg.lcores.begin(), cfg.lcores.end(), t) != cfg.lcores.end())
                    fail(fmt::format("lcore {} is in both --tx-masters and --lcores", t));
            }
        }
        if (cfg.dist_deal == DistDeal::Lb && cfg.dist_order != DistOrder::Seqn)
            fail("--dist-deal lb needs --dist-order seqn (round-robin collection cannot follow it)");
        if (cfg.dist_chunk == 0 || cfg.dist_chunk > MAX_BURST)
            fail(fmt::format("--dist-chunk {} must be in 1..{}", cfg.dist_chunk, MAX_BURST));
        if (cfg.dist_self > MAX_BURST)
            fail(fmt::format("--dist-self {} must be at most {}", cfg.dist_self, MAX_BURST));
        if (cfg.dist_self_backlog == 0 || cfg.dist_self_backlog + cfg.rx_burst >= cfg.dist_ring)
            fail(fmt::format("--dist-self-backlog {} must be in 1..{} (ring - rx-burst - 1)", cfg.dist_self_backlog,
                             cfg.dist_ring - cfg.rx_burst - 1));
        if (cfg.dist_self_idle > 100)
            fail(fmt::format("--dist-self-idle {} is a percentage", cfg.dist_self_idle));
        if (!is_pow2(cfg.dist_ring) || cfg.dist_ring < 4u * cfg.rx_burst || cfg.dist_ring > 65536)
            fail(fmt::format("--dist-ring {} must be a power of 2 in {}..65536 (4 x rx-burst)", cfg.dist_ring,
                             4u * cfg.rx_burst));
    } else if (!cfg.tx_masters.empty()) {
        fail("--tx-masters needs --masters");
    } else if (cfg.dist_order != DistOrder::Rr || cfg.dist_deal != DistDeal::Rr || cfg.dist_chunk != 32 ||
               cfg.dist_ring != 2048 || cfg.dist_self != 0) {
        warn("--dist-order/--dist-deal/--dist-chunk/--dist-ring have no effect without --masters");
    }
    const unsigned grp = cfg.musq_group_eff();
    const unsigned ntxq = cfg.nb_tx_queues();
    const unsigned ngrp = cfg.musq_groups;
    if (!is_pow2(ngrp) || ngrp > MUSQ_MAX_GROUPS) {
        fail(fmt::format("--musq-groups {} must be a power of 2 <= {}", ngrp, MUSQ_MAX_GROUPS));
        return false;
    }
    if (ngrp > 1) {
        /* Group g = queues g*N..g*N+N-1 on both sides: one RX and one TX queue
         * per lcore, so the queue a packet is steered to is the lane that
         * sends it back. */
        if (cfg.role != Role::Responder)
            fail("--musq-groups is a responder option");
        if (cfg.dist())
            fail("--musq-groups needs mu_sq lanes, not --masters");
        if (cfg.rx_queues > 1 || cfg.tx_lanes > 1)
            fail("--musq-groups > 1 needs one RX and one TX queue per lcore (no --rx-queues/--tx-lanes)");
        if (nw % ngrp != 0 || !is_pow2(nw / ngrp))
            fail(fmt::format("--musq-groups {}: {} lcores do not form {} groups of a power-of-2 lanes", ngrp, nw, ngrp));
        if (!cfg.doca_enabled())
            warn("--musq-groups with plain RSS: a flow stays on one lane, nothing is round-robinned inside a group "
                 "(PMD smoke test only; the hash -> group -> round robin steering needs --doca-mode esp_rr)");
        else if (cfg.doca_mode == DocaMode::MetaRr)
            fail("--musq-groups > 1 needs --doca-mode esp_rr (meta_rr's NIC counter cannot serve several groups)");
        if (grp == 0)
            warn(fmt::format("--musq-group 0 with --musq-groups {}: mu_sq off, the NIC still steers {} groups of {} "
                             "queues (steering-only test)", ngrp, ngrp, cfg.group_rx_queues()));
    }
    if (!cfg.hash_probe_src.empty()) {
        struct in_addr a;
        if (inet_pton(AF_INET, cfg.hash_probe_src.c_str(), &a) != 1 ||
            inet_pton(AF_INET, cfg.hash_probe_dst.c_str(), &a) != 1)
            fail("--hash-probe: bad IPv4 address");
        if (!cfg.doca_enabled() || cfg.role != Role::Responder)
            fail("--hash-probe needs the responder role and a DOCA mode (the hash is the steering pipeline's)");
    }
    if (grp != 0 && (!is_pow2(grp) || grp > MUSQ_MAX_GROUP))
        fail(fmt::format("--musq-group {} must be 0 or a power of 2 <= {}", grp, MUSQ_MAX_GROUP));
    if (grp != 0 && grp * ngrp != ntxq) {
        if (ntxq == 1 && ngrp == 1)
            warn(fmt::format("--musq-group {} with one TX queue: that queue drives all {} lanes itself "
                             "(needs the prom-dpdk mlx5 PMD that supports it)", grp, grp));
        else
            /* The PMD derives the group count from the TX queues and the
             * group size: anything else aliases the first group's slots. */
            fail(fmt::format("--musq-group {} x --musq-groups {} = {} lanes, but {} TX queues; a group needs one "
                             "TX queue per lane", grp, ngrp, grp * ngrp, ntxq));
    }

    if (!is_pow2(cfg.tx_burst) || cfg.tx_burst > MAX_BURST)
        fail(fmt::format("--tx-burst {} must be a power of 2 <= {}", cfg.tx_burst, MAX_BURST));
    /* A responder in accumulate mode buffers and sends tx_burst packets at a
     * time whatever the RX burst size (e.g. 64-packet RX bursts into 32-packet
     * mu_sq slots); in immediate mode one RX burst is one TX call. */
    const bool big_rx_ok = cfg.role == Role::Responder && cfg.tx_mode == TxMode::Accumulate;
    if (cfg.rx_burst == 0 || cfg.rx_burst > (big_rx_ok ? MAX_BURST : cfg.tx_burst))
        fail(big_rx_ok ? fmt::format("--rx-burst {} must be in 1..{}", cfg.rx_burst, MAX_BURST)
                       : fmt::format("--rx-burst {} must be in 1..tx-burst ({})", cfg.rx_burst, cfg.tx_burst));
    if (grp != 0 && cfg.pkts_per_wqe == 32 && cfg.tx_burst > MUSQ_EMPW_PKTS)
        warn(fmt::format("--tx-burst {} exceeds the mu_sq slot size ({}); one tx_burst call will span slots",
                         cfg.tx_burst, MUSQ_EMPW_PKTS));
    if (cfg.pkts_per_wqe != 1 && cfg.pkts_per_wqe != 32)
        fail("--pkts-per-wqe must be 1 or 32");
    if (cfg.db_wqes != 0 && (grp == 0 || cfg.role != Role::Responder))
        fail("--db-wqes needs the responder role and a mu_sq group");
    if (cfg.db_wqes > 4096)
        fail("--db-wqes must be in 0..4096");
    if (cfg.lane_epoch_us != 0 && cfg.role != Role::Responder)
        fail("--lane-epoch-us is a responder option");
    if (cfg.lane_epoch_us != 0 && (cfg.lane_epoch_max == 0 || cfg.lane_epoch_out.empty()))
        fail("--lane-epoch-us needs --lane-epoch-max > 0 and an output file");
    if (cfg.tx_mode == TxMode::Accumulate && cfg.tx_drain_us == 0)
        fail("--tx-drain-us must be > 0 in accumulate mode");
    if (cfg.rxd < 64 || cfg.txd < 64)
        fail("--rxd/--txd must be >= 64");
    if (cfg.stats_interval_s <= 0)
        fail("--stats-interval must be > 0");
    if (cfg.work.kind == WorkKind::Header && cfg.doca_mode == DocaMode::EspRr)
        warn("--work header expects UDP requests, but esp_rr steers only IPv4/ESP traffic");
    if (cfg.role == Role::Requester) {
        if (cfg.doca_enabled() || cfg.musq_group_eff() != 0)
            fail("--role requester runs on the traffic-generator host: use --doca-mode none --musq-group 0");
        if (cfg.rps == 0)
            fail("--rps must be > 0");
        if (!is_pow2(cfg.tracker_size) || cfg.tracker_size < 1024)
            fail("--tracker-size must be a power of 2 >= 1024");
        const unsigned min_content = 14 + 20 + 8 + 18; /* eth + ipv4 + (udp | spi+sn) + request_header */
        if (cfg.frame_size < min_content + 4 || cfg.frame_size > 1518)
            fail(fmt::format("--frame-size {} must be in {}..1518 (on the wire incl. FCS)", cfg.frame_size, min_content + 4));
        if (!cfg.tx_file.empty() && cfg.frame_size < min_content + 4 + 8 + 1)
            fail(fmt::format("--tx-file needs --frame-size >= {} (room for the 8 B chunk index and data)", min_content + 4 + 9));
        if (!cfg.rx_record.empty() && cfg.tx_file.empty())
            fail("--rx-record needs --tx-file");
        if (!cfg.src_ips.empty()) {
            if (cfg.src_ips.size() != nw)
                fail(fmt::format("--src-ips lists {} addresses for {} lcores", cfg.src_ips.size(), nw));
            if (cfg.rx_steer == RxSteer::None)
                fail("--src-ips gives every lcore its own source IP: not with --rx-steer none");
            struct in_addr a;
            for (const std::string &ip : cfg.src_ips)
                if (inet_pton(AF_INET, ip.c_str(), &a) != 1)
                    fail(fmt::format("--src-ips: bad IPv4 address '{}'", ip));
        }
        if (cfg.esp_hw) {
            if (cfg.esp_sa_per_lcore && cfg.rx_steer == RxSteer::None && nw > 1)
                fail("--esp-sa lcore gives every lcore its own SA by its source IP: needs --rx-steer flow|rss or "
                     "--src-ips (or --esp-sa port)");
            /* IPv4 + ESP hdr + IV + (UDP + request header, padded) + trailer + ICV */
            const unsigned min_hw = 14 + 20 + 16 + 28 + 16 + 4;
            if (cfg.traffic != TrafficKind::Esp)
                fail("--esp-encap hw needs --traffic esp");
            if (cfg.frame_size < min_hw)
                fail(fmt::format("--esp-encap hw needs --frame-size >= {}", min_hw));
            if (!cfg.tx_file.empty())
                fail("--esp-encap hw cannot be combined with --tx-file (the NIC, not the file, sets the sequence number)");
#if !ENABLE_ESP_HW
            fail("--esp-encap hw needs a build with -Desp_hw=enabled (scripts/run.sh picks build-esphw)");
#endif
        }
    } else if (!cfg.tx_file.empty() || !cfg.rx_record.empty()) {
        fail("--tx-file/--rx-record are requester options");
    } else if (cfg.esp_hw) {
        fail("--esp-encap is a requester option");
    }
    return ok;
}

std::string musq_devargs(const AppConfig &cfg)
{
    std::string s;
    const unsigned grp = cfg.musq_group_eff();
    if (grp != 0) {
        s = fmt::format("mu_sq_log_grp_size={}", log2_pow2(grp));
        if (cfg.db_wqes != 0)
            s += fmt::format(",mu_sq_db_wqes={}", cfg.db_wqes);
    } else if (cfg.pkts_per_wqe == 1) {
        s = "txq_mpw_en=0"; /* no multi-packet WQEs: one SEND WQE per packet */
    }
    if (!cfg.devargs.empty()) {
        if (!s.empty())
            s += ",";
        s += cfg.devargs;
    }
    return s;
}

std::vector<std::string> build_eal_argv(const AppConfig &cfg, const char *prog)
{
    std::vector<std::string> v;
    v.emplace_back(prog);
    std::string l = fmt::format("{}", cfg.main_lcore);
    for (unsigned c : cfg.masters)
        l += fmt::format(",{}", c);
    for (unsigned c : cfg.tx_masters)
        l += fmt::format(",{}", c);
    for (unsigned c : cfg.lcores)
        l += fmt::format(",{}", c);
    v.emplace_back("-l");
    v.push_back(l);
    v.emplace_back("--main-lcore");
    v.push_back(fmt::format("{}", cfg.main_lcore));
    v.emplace_back("--file-prefix");
    v.push_back(cfg.file_prefix);
    v.emplace_back("--log-level");
    v.push_back(fmt::format("{}", cfg.log_level));
    if (cfg.doca_enabled() || cfg.esp_hw) {
        /* Same shape l3fwd's pre_parse_args() produces: a dummy BDF switches EAL
         * to allow-list mode so the real PF is not attached by EAL, and the
         * auxiliary bus stays scanned. The PF is probed through the DOCA DPDK
         * bridge after rte_eal_init (see doca_probe in bringup.cpp). */
        v.emplace_back("-a");
        v.emplace_back("0000:00:00.0");
        v.emplace_back("-a");
        v.emplace_back("auxiliary:");
    } else {
        const std::string d = musq_devargs(cfg);
        v.emplace_back("-a");
        v.push_back(d.empty() ? cfg.pci : cfg.pci + "," + d);
    }
    std::stringstream ss(cfg.eal_args);
    std::string tok;
    while (ss >> tok)
        v.push_back(tok);
    return v;
}

void print_resolved_config(const AppConfig &cfg, const std::vector<std::string> &eal_argv)
{
    std::string lc;
    for (unsigned c : cfg.lcores)
        lc += (lc.empty() ? "" : ",") + fmt::format("{}", c);
    printf("musq-testapp config:\n");
    printf("  pci=%s devargs='%s' musq_group=%u musq_groups=%u doca_mode=%s doca_group=%u\n",
           cfg.pci.c_str(), musq_devargs(cfg).c_str(), cfg.musq_group_eff(), cfg.musq_groups,
           doca_mode_name(cfg.doca_mode), cfg.doca_group);
    if (cfg.musq_groups > 1)
        printf("  groups: %u x %u queues (group g = RX/TX queues g*%u..); hash_probe=%s\n", cfg.musq_groups,
               cfg.group_rx_queues(), cfg.group_rx_queues(),
               cfg.hash_probe_src.empty() ? "off"
                   : fmt::format("{}+{}:{}", cfg.hash_probe_src, cfg.hash_probe_n, cfg.hash_probe_dst).c_str());
    printf("  main_lcore=%u workers=[%s] role=%s pkts_per_wqe=%u rx_queues=%u tx_lanes=%u db_wqes=%u\n", cfg.main_lcore,
           lc.c_str(), role_name(cfg.role), cfg.pkts_per_wqe, cfg.rx_queues, cfg.tx_lanes, cfg.db_wqes);
    if (cfg.dist()) {
        std::string ms;
        for (unsigned c : cfg.masters)
            ms += (ms.empty() ? "" : ",") + fmt::format("{}", c);
        std::string ts;
        for (unsigned c : cfg.tx_masters)
            ts += (ts.empty() ? "" : ",") + fmt::format("{}", c);
        printf("  masters=[%s] tx_masters=[%s] (%u workers each) dist_order=%s dist_deal=%s dist_chunk=%u dist_ring=%u "
               "dist_self=%u dist_self_backlog=%u dist_self_idle=%u%%\n",
               ms.c_str(), ts.c_str(), cfg.workers_per_master(), dist_order_name(cfg.dist_order),
               dist_deal_name(cfg.dist_deal), cfg.dist_chunk, cfg.dist_ring, cfg.dist_self, cfg.dist_self_backlog,
               cfg.dist_self_idle);
    }
    printf("  rx_burst=%u tx_burst=%u tx_mode=%s tx_drain_us=%lu rxd=%u txd=%u pool=%s promisc=%d\n",
           cfg.rx_burst, cfg.tx_burst, tx_mode_name(cfg.tx_mode),
           static_cast<unsigned long>(cfg.tx_drain_us), cfg.rxd, cfg.txd,
           cfg.per_lcore_pool ? "per-lcore" : "shared", cfg.promisc ? 1 : 0);
    if (cfg.role == Role::Requester)
        printf("  rps/lcore=%lu (total %lu) traffic=%s frame=%uB service=%s dst_mac=%s %s%s->%s sport=%u-%u dport=%u-%u slots=%u rx_steer=%s tx_fill=%s\n",
               static_cast<unsigned long>(cfg.rps), static_cast<unsigned long>(cfg.rps * cfg.nb_workers()),
               cfg.traffic == TrafficKind::Udp ? "udp" : "esp", cfg.frame_size,
               work_spec_to_string(cfg.service_time).c_str(), cfg.dst_mac.c_str(), cfg.src_ip.c_str(),
               cfg.rx_steer == RxSteer::None ? "" : "+idx",
               cfg.dst_ip.c_str(), cfg.sport_lo, cfg.sport_hi, cfg.dport_lo, cfg.dport_hi, cfg.tracker_size,
               rx_steer_name(cfg.rx_steer), tx_fill_name(cfg.tx_fill));
    if (cfg.role == Role::Requester && !cfg.tx_file.empty())
        printf("  tx_file='%s' rx_record='%s'\n", cfg.tx_file.c_str(), cfg.rx_record.c_str());
    if (cfg.role == Role::Requester && cfg.traffic == TrafficKind::Esp)
        printf("  esp_encap=%s esp_sa=%s esp_spi=0x%x%s sn_check=%s%s\n",
               cfg.esp_hw ? "hw (NIC IPsec packet offload)" : "sw", cfg.esp_sa_per_lcore ? "lcore" : "port",
               cfg.esp_spi, cfg.esp_sa_per_lcore ? "+idx" : "", cfg.esp_sn_check ? "on" : "off",
               cfg.src_ips.empty() ? "" : " src_ips=explicit");
    if (cfg.role != Role::Requester)
        printf("  work=%s swap=%s\n", work_spec_to_string(cfg.work).c_str(), swap_mode_name(cfg.swap));
    printf("  duration=%.1fs warmup=%.1fs stats_interval=%.2fs xstats=%s csv='%s'\n",
           cfg.duration_s, cfg.warmup_s, cfg.stats_interval_s, xstats_mode_name(cfg.xstats),
           cfg.stats_csv.c_str());
    printf("  eal argv:");
    for (const auto &a : eal_argv)
        printf(" %s", a.c_str());
    printf("\n");
    if (cfg.doca_enabled())
        printf("  doca probe: pci=%s devargs='%s' (DOCA_PIPELINES_MODE=%s)\n",
               cfg.pci.c_str(), musq_devargs(cfg).c_str(), doca_mode_name(cfg.doca_mode));
}
