#include "config.hpp"

#include <cxxopts.hpp>
#include <fmt/format.h>

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
    std::string lcores, doca_mode, tx_mode, work, swap, xstats, role, musq_group;
    bool shared_pool = false, no_promisc = false;
    opts.add_options()
        ("pci", "PCI BDF of the data-path port", cxxopts::value<std::string>()->default_value(cfg.pci))
        ("devargs", "Extra mlx5 devargs (appended after mu_sq_log_grp_size)", cxxopts::value<std::string>()->default_value(""))
        ("musq-group", "mu_sq group size: auto (= number of workers), 0 (off) or a power of 2 <= 128", cxxopts::value<std::string>()->default_value("auto"))
        ("l,lcores", "Worker lcores, e.g. 2,4,6,8 or 2-8", cxxopts::value<std::string>())
        ("main-lcore", "Lcore running EAL main + stats (not a worker)", cxxopts::value<unsigned>()->default_value("0"))
        ("doca-mode", "none | meta_rr | esp_rr (DOCA Flow steering pipeline)", cxxopts::value<std::string>()->default_value("esp_rr"))
        ("doca-group", "DOCA round-robin steering group size (power of 2)", cxxopts::value<uint16_t>()->default_value("1"))
        ("rx-burst", "rte_eth_rx_burst size (<= tx-burst)", cxxopts::value<uint16_t>()->default_value("32"))
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
    if (!parse_enum<RxSteer>(r["rx-steer"].as<std::string>(),
            {{"flow", RxSteer::Flow}, {"rss", RxSteer::Rss}, {"none", RxSteer::None}}, cfg.rx_steer)) {
        fprintf(stderr, "error: --rx-steer must be flow|rss|none\n");
        return -1;
    }
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
    const unsigned grp = cfg.musq_group_eff();
    if (grp != 0 && (!is_pow2(grp) || grp > MUSQ_MAX_GROUP))
        fail(fmt::format("--musq-group {} must be 0 or a power of 2 <= {}", grp, MUSQ_MAX_GROUP));
    if (grp != 0 && grp != nw)
        warn(fmt::format("--musq-group {} != {} worker lcores; the convention is one lane per worker", grp, nw));

    if (!is_pow2(cfg.tx_burst) || cfg.tx_burst > MAX_BURST)
        fail(fmt::format("--tx-burst {} must be a power of 2 <= {}", cfg.tx_burst, MAX_BURST));
    if (cfg.rx_burst == 0 || cfg.rx_burst > cfg.tx_burst)
        fail(fmt::format("--rx-burst {} must be in 1..tx-burst ({})", cfg.rx_burst, cfg.tx_burst));
    if (grp != 0 && cfg.tx_burst > MUSQ_EMPW_PKTS)
        warn(fmt::format("--tx-burst {} exceeds the mu_sq slot size ({}); one tx_burst call will span slots",
                         cfg.tx_burst, MUSQ_EMPW_PKTS));
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
    }
    return ok;
}

std::string musq_devargs(const AppConfig &cfg)
{
    std::string s;
    const unsigned grp = cfg.musq_group_eff();
    if (grp != 0)
        s = fmt::format("mu_sq_log_grp_size={}", log2_pow2(grp));
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
    if (cfg.doca_enabled()) {
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
    printf("  pci=%s devargs='%s' musq_group=%u doca_mode=%s doca_group=%u\n",
           cfg.pci.c_str(), musq_devargs(cfg).c_str(), cfg.musq_group_eff(),
           doca_mode_name(cfg.doca_mode), cfg.doca_group);
    printf("  main_lcore=%u workers=[%s] role=%s\n", cfg.main_lcore, lc.c_str(), role_name(cfg.role));
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
    else
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
