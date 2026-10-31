#pragma once

#include "app.hpp"

#include <string>
#include <vector>

/* Parse the command line into cfg. Returns 0, or 1 when the program should exit
 * successfully (--help), or -1 on error (message already printed). */
int parse_cli(int argc, char **argv, AppConfig &cfg);

/* Cross-option validation; prints errors/warnings. false = do not start. */
bool validate_config(const AppConfig &cfg);

/* EAL argv (argv[0] = prog). In DOCA mode the real PF is hidden from EAL
 * (-a 0000:00:00.0 -a auxiliary:) and probed later via the DOCA bridge. */
std::vector<std::string> build_eal_argv(const AppConfig &cfg, const char *prog);

/* "mu_sq_log_grp_size=<log2>[,<extra>]" or just "<extra>" / "" when mu_sq is off. */
std::string musq_devargs(const AppConfig &cfg);

std::string work_spec_to_string(const WorkSpec &w);
const char *doca_mode_name(DocaMode m);
const char *tx_mode_name(TxMode m);
const char *swap_mode_name(SwapMode m);
const char *role_name(Role r);
const char *dist_order_name(DistOrder o);
const char *dist_deal_name(DistDeal d);
const char *rx_steer_name(RxSteer s);
const char *tx_fill_name(TxFill f);
const char *xstats_mode_name(XstatsMode m);

void print_resolved_config(const AppConfig &cfg, const std::vector<std::string> &eal_argv);
