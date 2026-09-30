#include "stats.hpp"
#include "config.hpp"

#include <fmt/format.h>

#include <rte_cycles.h>

#include <hdr/hdr_histogram.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <functional>

/* ── formatting helpers (korba stats_report.cpp) ─────────────────────────── */

static std::string group_u64(uint64_t n)
{
    std::string s = fmt::format("{}", n);
    const std::size_t len = s.size();
    std::string r;
    r.reserve(len + len / 3);
    for (std::size_t i = 0; i < len; ++i) {
        if (i > 0 && (len - i) % 3 == 0)
            r.push_back(',');
        r.push_back(s[i]);
    }
    return r;
}

static std::string fmt_bandwidth(uint64_t delta_bytes, double window_s)
{
    if (window_s <= 0.0 || delta_bytes == 0)
        return "-";
    const double bps = static_cast<double>(delta_bytes) * 8.0 / window_s;
    if (bps >= 1e9) return fmt::format("{:.2f} Gbps", bps / 1e9);
    if (bps >= 1e6) return fmt::format("{:.2f} Mbps", bps / 1e6);
    if (bps >= 1e3) return fmt::format("{:.2f} Kbps", bps / 1e3);
    return fmt::format("{:.0f} bps", bps);
}

static std::string fmt_total_delta(uint64_t total, uint64_t delta)
{
    if (delta > 0)
        return fmt::format("{} (+{})", group_u64(total), group_u64(delta));
    return group_u64(total);
}

static std::string fmt_counter(uint64_t total, uint64_t delta, uint64_t pps)
{
    std::string s = fmt_total_delta(total, delta);
    if (pps > 0)
        s += fmt::format("  [{} pps]", group_u64(pps));
    return s;
}

/* Two-column label | value table (korba GlobalTable). */
class KvTable {
public:
    void add(const char *label, const std::string &value) { rows_.push_back({label, value}); }
    bool empty() const { return rows_.empty(); }
    void print(const char *title, FILE *fp = stdout) const
    {
        if (rows_.empty())
            return;
        std::size_t lw = 0, vw = 0;
        for (const auto &r : rows_) {
            lw = std::max(lw, r.first.size());
            vw = std::max(vw, r.second.size());
        }
        if (title)
            fprintf(fp, "\n=== %s ===\n", title);
        const std::string sep = std::string(lw, '-') + "-+-" + std::string(vw, '-');
        fprintf(fp, "%-*s | %s\n", static_cast<int>(lw), "Metric", "Value");
        fprintf(fp, "%s\n", sep.c_str());
        for (const auto &r : rows_)
            fprintf(fp, "%-*s | %s\n", static_cast<int>(lw), r.first.c_str(), r.second.c_str());
    }

private:
    std::vector<std::pair<std::string, std::string>> rows_;
};

/* Metrics as rows, cores as columns (korba StatsTable, reduced). */
class CoreTable {
public:
    void add_column(const std::string &hdr) { headers_.push_back(hdr); }
    void add_row(const std::string &label, const std::vector<std::string> &cells)
    {
        rows_.push_back({label, cells});
    }
    void print(FILE *fp = stdout) const
    {
        if (headers_.empty()) {
            fprintf(fp, "  (no active cores)\n");
            return;
        }
        const std::size_t ncols = headers_.size() + 1;
        std::vector<std::size_t> widths(ncols, 0);
        widths[0] = std::strlen("Metric");
        for (std::size_t j = 0; j < headers_.size(); j++)
            widths[j + 1] = headers_[j].size();
        for (const auto &r : rows_) {
            widths[0] = std::max(widths[0], r.first.size());
            for (std::size_t j = 0; j < r.second.size() && j + 1 < ncols; j++)
                widths[j + 1] = std::max(widths[j + 1], r.second[j].size());
        }
        fprintf(fp, "%-*s", static_cast<int>(widths[0]), "Metric");
        for (std::size_t j = 0; j < headers_.size(); j++)
            fprintf(fp, " | %*s", static_cast<int>(widths[j + 1]), headers_[j].c_str());
        fprintf(fp, "\n");
        std::string sep(widths[0], '-');
        for (std::size_t j = 1; j < ncols; j++)
            sep += "-+-" + std::string(widths[j], '-');
        fprintf(fp, "%s\n", sep.c_str());
        for (const auto &r : rows_) {
            fprintf(fp, "%-*s", static_cast<int>(widths[0]), r.first.c_str());
            for (std::size_t j = 0; j < r.second.size() && j + 1 < ncols; j++)
                fprintf(fp, " | %*s", static_cast<int>(widths[j + 1]), r.second[j].c_str());
            fprintf(fp, "\n");
        }
    }

private:
    std::vector<std::string> headers_;
    std::vector<std::pair<std::string, std::vector<std::string>>> rows_;
};

/* ── StatsReporter ───────────────────────────────────────────────────────── */

CoreSnapshot StatsReporter::snapshot(const WorkerCtx &w) const
{
    CoreSnapshot s;
    const WorkerStats &ws = w.stats;
#define LD(f) __atomic_load_n(&ws.f, __ATOMIC_RELAXED)
    s.rx_pkts = LD(rx_pkts);
    s.tx_pkts = LD(tx_pkts);
    s.tx_drops = LD(tx_drops);
    s.rx_bursts = LD(rx_bursts);
    s.rx_full_bursts = LD(rx_full_bursts);
    s.non_ipv4 = LD(non_ipv4);
    s.work_cycles = LD(work_cycles);
    s.cycles_used = LD(cycles_used);
    s.cycles_wasted = LD(cycles_wasted);
    s.req_sent = LD(req_sent);
    s.resp_matched = LD(resp_matched);
    s.resp_unmatched = LD(resp_unmatched);
    s.req_lost = LD(req_lost);
    s.tx_batch_capped = LD(tx_batch_capped);
#undef LD
    return s;
}

int StatsReporter::init(AppState &st)
{
    st_ = &st;
    hz_ = st.tsc_hz;
    const uint16_t port = st.port_id;

    const int n = rte_eth_xstats_get_names(port, nullptr, 0);
    if (n > 0) {
        std::vector<struct rte_eth_xstat_name> names(n);
        std::vector<uint64_t> ids(n);
        if (rte_eth_xstats_get_names(port, names.data(), n) == n) {
            for (int i = 0; i < n; i++) {
                xs_names_.emplace_back(names[i].name);
                xs_ids_.push_back(static_cast<uint64_t>(i));
                if (strcmp(names[i].name, "rx_phy_packets") == 0)
                    sum_ids_[0] = i;
                else if (strcmp(names[i].name, "rx_prio0_buf_discard_packets") == 0)
                    sum_ids_[1] = i;
            }
            xs_base_.assign(n, 0);
            xs_prev_.assign(n, 0);
        }
    }
    core_base_.assign(st.nb_workers, CoreSnapshot{});
    core_prev_.assign(st.nb_workers, CoreSnapshot{});

    if (!st.cfg.stats_csv.empty()) {
        csv_ = fopen(st.cfg.stats_csv.c_str(), "a");
        if (csv_ == nullptr) {
            APP_LOG_ERR("cannot open %s for writing", st.cfg.stats_csv.c_str());
            return -1;
        }
        if (ftell(csv_) == 0)
            fprintf(csv_, "timestamp_s,window_index,elapsed_s,window_s,is_final,scope,id,metric,total,delta,rate\n");
    }
    rebase();
    return 0;
}

void StatsReporter::rebase()
{
    start_tsc_ = rte_rdtsc();
    window_index_ = 0;
    rte_eth_stats_get(st_->port_id, &port_base_);
    port_prev_ = port_base_;
    std::vector<uint64_t> sv;
    if (read_summary_xstats(sv))
        for (int k = 0; k < 2; k++)
            sum_base_[k] = sum_prev_[k] = sv[k];
    if (!xs_ids_.empty()) {
        std::vector<uint64_t> vals(xs_ids_.size());
        if (rte_eth_xstats_get_by_id(st_->port_id, xs_ids_.data(), vals.data(),
                                     static_cast<unsigned>(xs_ids_.size())) == static_cast<int>(xs_ids_.size())) {
            xs_base_ = vals;
            xs_prev_ = vals;
        }
    }
    for (unsigned i = 0; i < st_->nb_workers; i++) {
        core_base_[i] = snapshot(st_->workers[i]);
        core_prev_[i] = core_base_[i];
        if (st_->workers[i].rtt_hist)
            hdr_reset(st_->workers[i].rtt_hist);
    }
}

void StatsReporter::csv_row(const char *scope, const std::string &id, const char *metric,
                            uint64_t total, uint64_t delta, double rate)
{
    if (csv_ == nullptr)
        return;
    fprintf(csv_, "%ld,%lu,%.3f,%.3f,%d,%s,%s,%s,%lu,%lu,%.3f\n",
            static_cast<long>(time(nullptr)), static_cast<unsigned long>(window_index_),
            elapsed_s_, window_s_, is_final_ ? 1 : 0, scope, id.c_str(), metric,
            static_cast<unsigned long>(total), static_cast<unsigned long>(delta), rate);
}

void StatsReporter::report(uint64_t window_tsc, bool is_final)
{
    const uint64_t now = rte_rdtsc();
    window_index_++;
    elapsed_s_ = hz_ ? static_cast<double>(now - start_tsc_) / static_cast<double>(hz_) : 0.0;
    window_s_ = hz_ ? static_cast<double>(window_tsc) / static_cast<double>(hz_) : 0.0;
    is_final_ = is_final;

    printf("\n######## %s report #%lu: elapsed %.1fs, window %.3fs ########\n",
           is_final ? "FINAL" : "interval", static_cast<unsigned long>(window_index_), elapsed_s_, window_s_);
    struct rte_eth_stats ps;
    if (rte_eth_stats_get(st_->port_id, &ps) != 0)
        memset(&ps, 0, sizeof(ps));
    if (st_->cfg.role == Role::Requester)
        report_summary(ps, window_tsc, window_s_);
    else
        report_responder_summary(ps, window_tsc, window_s_);
    report_port(ps, window_tsc, window_s_, is_final);
    report_xstats(window_tsc, window_s_, is_final);
    report_cores(window_tsc, window_s_, is_final);
    if (st_->cfg.role == Role::Requester)
        report_rtt(is_final);
    fflush(stdout);
    if (csv_)
        fflush(csv_);
}

/* Requester headline: totals since measurement start, window rates, and the
 * end-to-end drops (requests sent that never came back, incl. in flight). */
void StatsReporter::report_summary(const struct rte_eth_stats &s, uint64_t window_tsc, double window_s)
{
    const struct rte_eth_stats &base = port_base_, &prev = port_prev_;
    auto rate = [&](uint64_t d) -> double {
        return window_tsc ? static_cast<double>(d) * hz_ / window_tsc : 0.0;
    };
    const uint64_t tx_t = s.opackets - base.opackets, rx_t = s.ipackets - base.ipackets;
    const uint64_t txb_t = s.obytes - base.obytes, rxb_t = s.ibytes - base.ibytes;
    const uint64_t tx_d = s.opackets - prev.opackets, rx_d = s.ipackets - prev.ipackets;
    const uint64_t txb_d = s.obytes - prev.obytes, rxb_d = s.ibytes - prev.ibytes;
    const uint64_t drops = tx_t > rx_t ? tx_t - rx_t : 0;
    /* +4 B FCS per packet: report line rate on the wire, like TRex. */
    const double tx_gbps = window_s > 0 ? (txb_d + 4.0 * tx_d) * 8 / window_s / 1e9 : 0;
    const double rx_gbps = window_s > 0 ? (rxb_d + 4.0 * rx_d) * 8 / window_s / 1e9 : 0;
    const double avg_tx_pps = elapsed_s_ > 0 ? tx_t / elapsed_s_ : 0;
    const double avg_rx_pps = elapsed_s_ > 0 ? rx_t / elapsed_s_ : 0;

    KvTable t;
    t.add("TX total", fmt::format("{} pkts  {} bytes", group_u64(tx_t), group_u64(txb_t)));
    t.add("RX total", fmt::format("{} pkts  {} bytes", group_u64(rx_t), group_u64(rxb_t)));
    t.add("Drops (TX-RX)", fmt::format("{}  ({:.4f}%)", group_u64(drops), tx_t ? 100.0 * drops / tx_t : 0.0));
    t.add("TX pps", fmt::format("{}  (run avg {})", group_u64(static_cast<uint64_t>(rate(tx_d))),
                                group_u64(static_cast<uint64_t>(avg_tx_pps))));
    t.add("RX pps", fmt::format("{}  (run avg {})", group_u64(static_cast<uint64_t>(rate(rx_d))),
                                group_u64(static_cast<uint64_t>(avg_rx_pps))));
    t.add("TX Gbps (wire)", fmt::format("{:.3f}", tx_gbps));
    t.add("RX Gbps (wire)", fmt::format("{:.3f}", rx_gbps));
    t.print(fmt::format("Requester summary (window {:.3f}s, since start {:.1f}s)", window_s, elapsed_s_).c_str());

    csv_row("summary", "port0", "drops", drops, 0, tx_t ? 100.0 * drops / tx_t : 0.0);
    csv_row("summary", "port0", "tx_gbps_wire", 0, 0, tx_gbps);
    csv_row("summary", "port0", "rx_gbps_wire", 0, 0, rx_gbps);
}

bool StatsReporter::read_summary_xstats(std::vector<uint64_t> &vals) const
{
    vals.assign(2, 0);
    bool any = false;
    for (int k = 0; k < 2; k++) {
        if (sum_ids_[k] < 0)
            continue;
        uint64_t id = static_cast<uint64_t>(sum_ids_[k]);
        if (rte_eth_xstats_get_by_id(st_->port_id, &id, &vals[k], 1) == 1)
            any = true;
    }
    return any;
}

/* Responder headline: forwarding rate (pps and wire Gbps) per window and on
 * average since the measurement start, what the port was offered, and where the
 * packets that were not forwarded were lost. */
void StatsReporter::report_responder_summary(const struct rte_eth_stats &s, uint64_t window_tsc, double window_s)
{
    const struct rte_eth_stats &base = port_base_, &prev = port_prev_;
    auto rate = [&](uint64_t d) -> double {
        return window_tsc ? static_cast<double>(d) * hz_ / window_tsc : 0.0;
    };
    auto avg = [&](uint64_t t) -> double { return elapsed_s_ > 0 ? t / elapsed_s_ : 0.0; };
    /* +4 B FCS per packet: line rate on the wire, as in the requester summary. */
    auto gbps = [](uint64_t bytes, uint64_t pkts, double secs) -> double {
        return secs > 0 ? (bytes + 4.0 * pkts) * 8 / secs / 1e9 : 0.0;
    };
    const uint64_t rx_t = s.ipackets - base.ipackets, tx_t = s.opackets - base.opackets;
    const uint64_t rx_d = s.ipackets - prev.ipackets, tx_d = s.opackets - prev.opackets;
    const uint64_t rxb_t = s.ibytes - base.ibytes, txb_t = s.obytes - base.obytes;
    const uint64_t rxb_d = s.ibytes - prev.ibytes, txb_d = s.obytes - prev.obytes;
    const uint64_t miss_t = s.imissed - base.imissed, miss_d = s.imissed - prev.imissed;
    const uint64_t unfwd_t = rx_t > tx_t ? rx_t - tx_t : 0;

    KvTable t;
    t.add("RX pps", fmt::format("{}  (run avg {})", group_u64(static_cast<uint64_t>(rate(rx_d))),
                                group_u64(static_cast<uint64_t>(avg(rx_t)))));
    t.add("TX pps", fmt::format("{}  (run avg {})", group_u64(static_cast<uint64_t>(rate(tx_d))),
                                group_u64(static_cast<uint64_t>(avg(tx_t)))));
    t.add("RX Gbps (wire)", fmt::format("{:.3f}  (run avg {:.3f})", gbps(rxb_d, rx_d, window_s),
                                        gbps(rxb_t, rx_t, elapsed_s_)));
    t.add("TX Gbps (wire)", fmt::format("{:.3f}  (run avg {:.3f})", gbps(txb_d, tx_d, window_s),
                                        gbps(txb_t, tx_t, elapsed_s_)));

    std::vector<uint64_t> sv;
    const bool have_x = read_summary_xstats(sv);
    uint64_t phy_t = 0, phy_d = 0, disc_t = 0, disc_d = 0;
    if (have_x) {
        phy_t = sv[0] - sum_base_[0];
        phy_d = sv[0] - sum_prev_[0];
        disc_t = sv[1] - sum_base_[1];
        disc_d = sv[1] - sum_prev_[1];
        sum_prev_[0] = sv[0];
        sum_prev_[1] = sv[1];
    }
    if (have_x && sum_ids_[0] >= 0)
        t.add("Offered at port pps", fmt::format("{}  (rx_phy_packets)", group_u64(static_cast<uint64_t>(rate(phy_d)))));
    if (have_x && sum_ids_[1] >= 0)
        t.add("NIC discards", fmt::format("{}  [{} pps]  (rx_prio0_buf_discard_packets)", group_u64(disc_t),
                                          group_u64(static_cast<uint64_t>(rate(disc_d)))));
    t.add("RX missed", fmt::format("{}  [{} pps]  (RX queue out of buffers)", group_u64(miss_t),
                                   group_u64(static_cast<uint64_t>(rate(miss_d)))));
    t.add("Not forwarded (RX-TX)", fmt::format("{}  ({:.4f}%)", group_u64(unfwd_t), rx_t ? 100.0 * unfwd_t / rx_t : 0.0));
    t.print(fmt::format("Responder summary (window {:.3f}s, since start {:.1f}s)", window_s, elapsed_s_).c_str());

    csv_row("summary", "port0", "rx_pps", rx_t, rx_d, rate(rx_d));
    csv_row("summary", "port0", "tx_pps", tx_t, tx_d, rate(tx_d));
    csv_row("summary", "port0", "rx_gbps_wire", 0, 0, gbps(rxb_d, rx_d, window_s));
    csv_row("summary", "port0", "tx_gbps_wire", 0, 0, gbps(txb_d, tx_d, window_s));
    if (have_x && sum_ids_[1] >= 0)
        csv_row("summary", "port0", "nic_discards", disc_t, disc_d, rate(disc_d));
    if (have_x && sum_ids_[0] >= 0)
        csv_row("summary", "port0", "offered_pps", phy_t, phy_d, rate(phy_d));
}

void StatsReporter::report_port(const struct rte_eth_stats &s_in, uint64_t window_tsc, double window_s, bool is_final)
{
    (void)is_final;
    struct rte_eth_stats s = s_in;
    struct rte_eth_stats &base = port_base_, &prev = port_prev_;
    const bool reset = s.ipackets < base.ipackets || s.opackets < base.opackets ||
                       s.ipackets < prev.ipackets || s.opackets < prev.opackets;
    if (reset) {
        APP_LOG_WARN("port %u statistics went backwards; rebasing", st_->port_id);
        base = s;
        prev = s;
    }
    auto rate = [&](uint64_t d) -> uint64_t {
        return window_tsc ? static_cast<uint64_t>((static_cast<unsigned __int128>(d) * hz_) / window_tsc) : 0;
    };
    const uint64_t rx_d = s.ipackets - prev.ipackets, tx_d = s.opackets - prev.opackets;
    const uint64_t rxb_d = s.ibytes - prev.ibytes, txb_d = s.obytes - prev.obytes;
    const uint64_t rx_pps = rate(rx_d), tx_pps = rate(tx_d);

    KvTable t;
    t.add("RX pkts", fmt_counter(s.ipackets - base.ipackets, rx_d, rx_pps));
    t.add("RX bytes", fmt::format("{}  [{}]", fmt_total_delta(s.ibytes - base.ibytes, rxb_d), fmt_bandwidth(rxb_d, window_s)));
    t.add("RX errors", fmt_total_delta(s.ierrors - base.ierrors, s.ierrors - prev.ierrors));
    t.add("RX missed", fmt_total_delta(s.imissed - base.imissed, s.imissed - prev.imissed));
    t.add("RX nombuf", fmt_total_delta(s.rx_nombuf - base.rx_nombuf, s.rx_nombuf - prev.rx_nombuf));
    t.add("TX pkts", fmt_counter(s.opackets - base.opackets, tx_d, tx_pps));
    t.add("TX bytes", fmt::format("{}  [{}]", fmt_total_delta(s.obytes - base.obytes, txb_d), fmt_bandwidth(txb_d, window_s)));
    t.add("TX errors", fmt_total_delta(s.oerrors - base.oerrors, s.oerrors - prev.oerrors));
    t.print(fmt::format("Port {} (window {:.3f}s)", st_->port_id, window_s).c_str());

    const std::string id = fmt::format("port{}", st_->port_id);
    csv_row("port", id, "rx_packets", s.ipackets - base.ipackets, rx_d, static_cast<double>(rx_pps));
    csv_row("port", id, "rx_bytes", s.ibytes - base.ibytes, rxb_d, window_s > 0 ? rxb_d / window_s : 0);
    csv_row("port", id, "rx_errors", s.ierrors - base.ierrors, s.ierrors - prev.ierrors, 0);
    csv_row("port", id, "rx_missed", s.imissed - base.imissed, s.imissed - prev.imissed, 0);
    csv_row("port", id, "rx_nombuf", s.rx_nombuf - base.rx_nombuf, s.rx_nombuf - prev.rx_nombuf, 0);
    csv_row("port", id, "tx_packets", s.opackets - base.opackets, tx_d, static_cast<double>(tx_pps));
    csv_row("port", id, "tx_bytes", s.obytes - base.obytes, txb_d, window_s > 0 ? txb_d / window_s : 0);
    csv_row("port", id, "tx_errors", s.oerrors - base.oerrors, s.oerrors - prev.oerrors, 0);
    prev = s;
}

static bool xstat_interesting(const std::string &n)
{
    static const char *keys[] = {"tx_", "rx_", "phy", "discard", "drop", "missed", "nombuf", "mu_"};
    for (const char *k : keys)
        if (n.find(k) != std::string::npos)
            return true;
    return false;
}

void StatsReporter::report_xstats(uint64_t window_tsc, double window_s, bool is_final)
{
    (void)window_s;
    if (xs_ids_.empty())
        return;
    const XstatsMode mode = st_->cfg.xstats;
    if (mode == XstatsMode::Final && !is_final)
        return;
    std::vector<uint64_t> vals(xs_ids_.size());
    if (rte_eth_xstats_get_by_id(st_->port_id, xs_ids_.data(), vals.data(),
                                 static_cast<unsigned>(xs_ids_.size())) != static_cast<int>(xs_ids_.size()))
        return;
    KvTable t;
    const std::string id = fmt::format("port{}", st_->port_id);
    for (std::size_t i = 0; i < vals.size(); i++) {
        const uint64_t total = vals[i] - xs_base_[i];
        const uint64_t delta = vals[i] - xs_prev_[i];
        xs_prev_[i] = vals[i];
        if (total == 0 && delta == 0)
            continue;
        if (mode == XstatsMode::Filtered && !xstat_interesting(xs_names_[i]))
            continue;
        const uint64_t rate = window_tsc ? static_cast<uint64_t>((static_cast<unsigned __int128>(delta) * hz_) / window_tsc) : 0;
        t.add(xs_names_[i].c_str(), fmt_counter(total, delta, rate));
        csv_row("xstat", id, xs_names_[i].c_str(), total, delta, static_cast<double>(rate));
    }
    t.print(fmt::format("Port {} xstats ({}, non-zero)", st_->port_id, xstats_mode_name(mode)).c_str());
}

void StatsReporter::report_cores(uint64_t window_tsc, double window_s, bool is_final)
{
    (void)is_final;
    const unsigned nw = st_->nb_workers;
    std::vector<CoreSnapshot> cur(nw), tot(nw), del(nw);
    for (unsigned i = 0; i < nw; i++) {
        cur[i] = snapshot(st_->workers[i]);
        const CoreSnapshot &b = core_base_[i], &p = core_prev_[i];
#define DIFF(f) tot[i].f = cur[i].f - b.f; del[i].f = cur[i].f - p.f
        DIFF(rx_pkts); DIFF(tx_pkts); DIFF(tx_drops); DIFF(rx_bursts); DIFF(rx_full_bursts);
        DIFF(non_ipv4); DIFF(work_cycles); DIFF(cycles_used); DIFF(cycles_wasted);
        DIFF(req_sent); DIFF(resp_matched); DIFF(resp_unmatched); DIFF(req_lost); DIFF(tx_batch_capped);
#undef DIFF
        core_prev_[i] = cur[i];
    }
    auto rate = [&](uint64_t d) -> uint64_t {
        return window_tsc ? static_cast<uint64_t>((static_cast<unsigned __int128>(d) * hz_) / window_tsc) : 0;
    };

    CoreTable t;
    for (unsigned i = 0; i < nw; i++)
        t.add_column(fmt::format("Core {}", st_->workers[i].lcore_id));
    auto row = [&](const char *label, std::function<std::string(unsigned)> fn) {
        std::vector<std::string> cells;
        for (unsigned i = 0; i < nw; i++)
            cells.push_back(fn(i));
        t.add_row(label, cells);
    };
    row("RX pps", [&](unsigned i) { return group_u64(rate(del[i].rx_pkts)); });
    row("TX pps", [&](unsigned i) { return group_u64(rate(del[i].tx_pkts)); });
    row("RX pkts", [&](unsigned i) { return group_u64(tot[i].rx_pkts); });
    row("  RX pkts (+win)", [&](unsigned i) { return group_u64(del[i].rx_pkts); });
    row("TX pkts", [&](unsigned i) { return group_u64(tot[i].tx_pkts); });
    row("  TX pkts (+win)", [&](unsigned i) { return group_u64(del[i].tx_pkts); });
    row("TX drops", [&](unsigned i) { return fmt_total_delta(tot[i].tx_drops, del[i].tx_drops); });
    const bool requester = st_->cfg.role == Role::Requester;
    if (requester) {
        row("Req sent", [&](unsigned i) { return fmt_total_delta(tot[i].req_sent, del[i].req_sent); });
        row("Resp matched", [&](unsigned i) { return fmt_total_delta(tot[i].resp_matched, del[i].resp_matched); });
        row("Resp unmatched", [&](unsigned i) { return fmt_total_delta(tot[i].resp_unmatched, del[i].resp_unmatched); });
        row("Req lost", [&](unsigned i) { return fmt_total_delta(tot[i].req_lost, del[i].req_lost); });
        row("Batches capped", [&](unsigned i) { return fmt_total_delta(tot[i].tx_batch_capped, del[i].tx_batch_capped); });
        row("Non-response RX", [&](unsigned i) { return fmt_total_delta(tot[i].non_ipv4, del[i].non_ipv4); });
    } else {
        row("Non-IPv4 pkts", [&](unsigned i) { return fmt_total_delta(tot[i].non_ipv4, del[i].non_ipv4); });
    }
    row("CPU busy %", [&](unsigned i) {
        const uint64_t all = del[i].cycles_used + del[i].cycles_wasted;
        return all ? fmt::format("{:.1f}%", 100.0 * del[i].cycles_used / all) : std::string("-");
    });
    row("CPU idle %", [&](unsigned i) {
        const uint64_t all = del[i].cycles_used + del[i].cycles_wasted;
        return all ? fmt::format("{:.1f}%", 100.0 * del[i].cycles_wasted / all) : std::string("-");
    });
    row("Cycles/pkt", [&](unsigned i) {
        const uint64_t n = requester ? del[i].rx_pkts + del[i].tx_pkts : del[i].rx_pkts;
        return n ? group_u64(del[i].cycles_used / n) : std::string("-");
    });
    if (!requester)
        row("Work us/pkt", [&](unsigned i) {
            const uint64_t n = del[i].rx_pkts;
            return n && hz_ ? fmt::format("{:.2f}", 1e6 * del[i].work_cycles / n / hz_) : std::string("-");
        });
    row("Avg RX burst", [&](unsigned i) {
        return del[i].rx_bursts ? fmt::format("{:.1f}", static_cast<double>(del[i].rx_pkts) / del[i].rx_bursts)
                                : std::string("-");
    });
    row("Full RX bursts", [&](unsigned i) { return fmt_total_delta(tot[i].rx_full_bursts, del[i].rx_full_bursts); });
    printf("\n=== Per-core (window %.3fs) ===\n", window_s);
    t.print();

    for (unsigned i = 0; i < nw; i++) {
        const std::string id = fmt::format("lcore{}", st_->workers[i].lcore_id);
        const uint64_t all = del[i].cycles_used + del[i].cycles_wasted;
        csv_row("core", id, "rx_packets", tot[i].rx_pkts, del[i].rx_pkts, static_cast<double>(rate(del[i].rx_pkts)));
        csv_row("core", id, "tx_packets", tot[i].tx_pkts, del[i].tx_pkts, static_cast<double>(rate(del[i].tx_pkts)));
        csv_row("core", id, "tx_drops", tot[i].tx_drops, del[i].tx_drops, 0);
        csv_row("core", id, "non_ipv4", tot[i].non_ipv4, del[i].non_ipv4, 0);
        csv_row("core", id, "rx_bursts", tot[i].rx_bursts, del[i].rx_bursts, 0);
        csv_row("core", id, "rx_full_bursts", tot[i].rx_full_bursts, del[i].rx_full_bursts, 0);
        csv_row("core", id, "cycles_used", tot[i].cycles_used, del[i].cycles_used, 0);
        csv_row("core", id, "cycles_wasted", tot[i].cycles_wasted, del[i].cycles_wasted, 0);
        csv_row("core", id, "work_cycles", tot[i].work_cycles, del[i].work_cycles, 0);
        csv_row("core", id, "cpu_busy_pct", 0, 0, all ? 100.0 * del[i].cycles_used / all : 0.0);
        if (requester) {
            csv_row("core", id, "req_sent", tot[i].req_sent, del[i].req_sent, static_cast<double>(rate(del[i].req_sent)));
            csv_row("core", id, "resp_matched", tot[i].resp_matched, del[i].resp_matched, static_cast<double>(rate(del[i].resp_matched)));
            csv_row("core", id, "resp_unmatched", tot[i].resp_unmatched, del[i].resp_unmatched, 0);
            csv_row("core", id, "req_lost", tot[i].req_lost, del[i].req_lost, 0);
            csv_row("core", id, "tx_batch_capped", tot[i].tx_batch_capped, del[i].tx_batch_capped, 0);
        }
    }
}

/* RTT percentiles (us), cumulative since the measurement start: merged over
 * all lcores plus one column per lcore. Histograms are read while the workers
 * record into them; a report may miss the last few samples, which is fine. */
void StatsReporter::report_rtt(bool is_final)
{
    (void)is_final;
    const unsigned nw = st_->nb_workers;
    struct hdr_histogram *all = nullptr;
    if (hdr_init(1, 10000000, 3, &all) != 0)
        return;
    for (unsigned i = 0; i < nw; i++)
        if (st_->workers[i].rtt_hist)
            hdr_add(all, st_->workers[i].rtt_hist);

    static const double pcts[] = {50.0, 90.0, 99.0, 99.9, 99.99};
    CoreTable t;
    t.add_column("All");
    for (unsigned i = 0; i < nw; i++)
        t.add_column(fmt::format("Core {}", st_->workers[i].lcore_id));
    auto row = [&](const char *label, std::function<std::string(struct hdr_histogram *)> fn) {
        std::vector<std::string> cells;
        cells.push_back(fn(all));
        for (unsigned i = 0; i < nw; i++)
            cells.push_back(st_->workers[i].rtt_hist ? fn(st_->workers[i].rtt_hist) : std::string("-"));
        t.add_row(label, cells);
    };
    auto val = [](struct hdr_histogram *h, int64_t v) { return h->total_count ? group_u64(static_cast<uint64_t>(v)) : std::string("-"); };
    row("Samples", [&](struct hdr_histogram *h) { return group_u64(static_cast<uint64_t>(h->total_count)); });
    row("Min us", [&](struct hdr_histogram *h) { return val(h, hdr_min(h)); });
    row("Mean us", [&](struct hdr_histogram *h) { return h->total_count ? fmt::format("{:.1f}", hdr_mean(h)) : std::string("-"); });
    for (double p : pcts)
        row(fmt::format("p{:g} us", p).c_str(), [&](struct hdr_histogram *h) { return val(h, hdr_value_at_percentile(h, p)); });
    row("Max us", [&](struct hdr_histogram *h) { return val(h, hdr_max(h)); });
    printf("\n=== RTT since measurement start (us) ===\n");
    t.print();

    if (all->total_count) {
        csv_row("rtt", "all", "samples", static_cast<uint64_t>(all->total_count), 0, 0);
        csv_row("rtt", "all", "min_us", static_cast<uint64_t>(hdr_min(all)), 0, 0);
        csv_row("rtt", "all", "mean_us", 0, 0, hdr_mean(all));
        for (double p : pcts)
            csv_row("rtt", "all", fmt::format("p{:g}_us", p).c_str(),
                    static_cast<uint64_t>(hdr_value_at_percentile(all, p)), 0, 0);
        csv_row("rtt", "all", "max_us", static_cast<uint64_t>(hdr_max(all)), 0, 0);
    }
    hdr_close(all);
}

void StatsReporter::close()
{
    if (csv_) {
        fclose(csv_);
        csv_ = nullptr;
    }
}

int lcore_usage_cb(unsigned int lcore_id, struct rte_lcore_usage *usage)
{
    if (lcore_id >= RTE_MAX_LCORE)
        return -1;
    const int widx = g_app.lcore_to_worker[lcore_id];
    if (widx < 0)
        return -1;
    const WorkerStats &s = g_app.workers[widx].stats;
    const uint64_t start = __atomic_load_n(&s.loop_start_tsc, __ATOMIC_RELAXED);
    usage->total_cycles = start ? rte_rdtsc() - start : 0;
    usage->busy_cycles = __atomic_load_n(&s.cycles_used, __ATOMIC_RELAXED);
    return 0;
}
