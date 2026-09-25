#pragma once

#include "app.hpp"

/* DOCA DPDK-bridge probe of the real PF (after rte_eal_init, before any
 * ethdev call). Creates ethdev port 0. */
int doca_probe(const AppConfig &cfg);
/* Install the DOCA Flow steering pipeline (after rte_eth_dev_start). */
int doca_init(const AppConfig &cfg, unsigned nb_workers);
void doca_cleanup();

/* Fill AppState::workers from the EAL worker lcores (queue id == worker index). */
int setup_workers(AppState &st);
int create_pools(AppState &st);
/* configure + queues + start + promisc + link wait, on st.port_id. */
int configure_port(AppState &st);
void stop_port(AppState &st);
void free_pools(AppState &st);
