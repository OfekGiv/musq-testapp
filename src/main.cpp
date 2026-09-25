#include "app.hpp"
#include "bringup.hpp"
#include "config.hpp"
#include "requester.hpp"
#include "responder.hpp"
#include "stats.hpp"

#include <rte_eal.h>
#include <rte_cycles.h>
#include <rte_launch.h>

#include <csignal>
#include <cstdlib>
#include <cstring>

AppState g_app;
static volatile bool g_run_loop_active = false;

/* First signal while the run loop is active asks for a clean stop; a signal
 * during bring-up, or a second one, terminates (korba main.cpp). */
static void signal_handler(int signum)
{
    if (signum != SIGINT && signum != SIGTERM)
        return;
    if (!g_run_loop_active || g_app.force_quit) {
        signal(signum, SIG_DFL);
        raise(signum);
        return;
    }
    g_app.force_quit = true;
}

static int worker_entry(void *arg)
{
    AppState *st = static_cast<AppState *>(arg);
    const int widx = st->lcore_to_worker[rte_lcore_id()];
    if (widx < 0)
        return 0;
    switch (st->workers[widx].role) {
    case Role::Responder:
        return responder_loop(arg);
    case Role::Requester:
        return requester_loop(arg);
    }
    return -1;
}

/* Main lcore: warmup, periodic reports, duration (korba core.cpp main_worker). */
static void monitor_loop(AppState &st, StatsReporter &rep)
{
    const AppConfig &cfg = st.cfg;
    const uint64_t hz = st.tsc_hz;
    const auto s_to_tsc = [hz](double s) { return static_cast<uint64_t>(s * static_cast<double>(hz)); };
    const uint64_t start = rte_rdtsc();
    const uint64_t duration_tsc = s_to_tsc(cfg.duration_s);
    const uint64_t warmup_tsc = s_to_tsc(cfg.warmup_s);
    const uint64_t interval_tsc = s_to_tsc(cfg.stats_interval_s);
    uint64_t prev_report = start;

    if (warmup_tsc == 0) {
        rep.rebase();
        st.measuring = true;
    }
    while (!st.force_quit) {
        rte_delay_ms(100);
        const uint64_t now = rte_rdtsc();
        if (duration_tsc && now - start >= duration_tsc) {
            APP_LOG_INFO("run duration reached (%.1fs), stopping", cfg.duration_s);
            st.force_quit = true;
            break;
        }
        if (!st.measuring && now - start >= warmup_tsc) {
            APP_LOG_INFO("warmup done (%.1fs), measurement starts", cfg.warmup_s);
            rep.rebase();
            st.measuring = true;
            prev_report = now;
            continue;
        }
        if (st.measuring && now - prev_report >= interval_tsc) {
            rep.report(now - prev_report, false);
            prev_report = now;
        }
    }
    /* Workers stop; the final report covers the tail window. */
    rte_eal_mp_wait_lcore();
    const uint64_t now = rte_rdtsc();
    if (!st.measuring) {
        rep.rebase();
        st.measuring = true;
    }
    rep.report(now - prev_report, true);
}

int main(int argc, char **argv)
{
    AppState &st = g_app;
    AppConfig &cfg = st.cfg;

    /* Keep stdout (config + stats tables) ordered with the RTE_LOG lines on
     * stderr when both are redirected to one file. */
    setvbuf(stdout, nullptr, _IOLBF, 0);

    const int pr = parse_cli(argc, argv, cfg);
    if (pr != 0)
        return pr < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
    if (!validate_config(cfg))
        return EXIT_FAILURE;

    const std::vector<std::string> eal = build_eal_argv(cfg, argv[0]);
    print_resolved_config(cfg, eal);
    if (cfg.dry_run)
        return EXIT_SUCCESS;

    /* The DOCA SDK reads this when the pipeline is installed; `env -i` runs
     * would otherwise lose it. */
    if (cfg.doca_enabled())
        setenv("DOCA_PIPELINES_MODE", doca_mode_name(cfg.doca_mode), 1);

    std::vector<char *> eal_argv;
    for (const auto &a : eal)
        eal_argv.push_back(strdup(a.c_str()));
    eal_argv.push_back(nullptr);
    int ret = rte_eal_init(static_cast<int>(eal.size()), eal_argv.data());
    if (ret < 0) {
        fprintf(stderr, "rte_eal_init failed: %s\n", rte_strerror(rte_errno));
        return EXIT_FAILURE;
    }
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    st.tsc_hz = rte_get_tsc_hz();
    st.port_id = 0;

    int rc = EXIT_FAILURE;
    bool doca_up = false, port_up = false;
    StatsReporter reporter;

    if (cfg.doca_enabled()) {
        if (doca_probe(cfg) < 0)
            goto out;
    } else if (rte_eth_dev_count_avail() < 1) {
        APP_LOG_ERR("no ethdev found for %s", cfg.pci.c_str());
        goto out;
    }
    if (setup_workers(st) < 0)
        goto out;
    if (create_pools(st) < 0)
        goto out;
    if (configure_port(st) < 0)
        goto out;
    port_up = true;
    if (cfg.doca_enabled()) {
        if (doca_init(cfg, st.nb_workers) < 0)
            goto out;
        doca_up = true;
    }
    if (cfg.role == Role::Requester) {
        if (requester_init(st) < 0)
            goto out;
    } else {
        responder_init(cfg, st.tsc_hz);
    }
    if (reporter.init(st) < 0)
        goto out;
    rte_lcore_register_usage_cb(lcore_usage_cb);

    g_run_loop_active = true;
    for (unsigned i = 0; i < st.nb_workers; i++) {
        ret = rte_eal_remote_launch(worker_entry, &st, st.workers[i].lcore_id);
        if (ret != 0) {
            APP_LOG_ERR("cannot launch worker on lcore %u: %s", st.workers[i].lcore_id, rte_strerror(-ret));
            st.force_quit = true;
            rte_eal_mp_wait_lcore();
            goto out;
        }
    }
    monitor_loop(st, reporter);
    rc = EXIT_SUCCESS;

out:
    st.force_quit = true;
    rte_eal_mp_wait_lcore();
    reporter.close();
    /* l3fwd order: ports first, DOCA (set up last) next, then EAL. */
    if (port_up)
        stop_port(st);
    if (doca_up || cfg.doca_enabled())
        doca_cleanup();
    requester_cleanup(st);
    free_pools(st);
    rte_eal_cleanup();
    /* eal_argv strings are intentionally not freed: rte_eal_init() takes
     * ownership of (and frees) some of them, so a free here double-frees. */
    APP_LOG_INFO("bye");
    return rc;
}
