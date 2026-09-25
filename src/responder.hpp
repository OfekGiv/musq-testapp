#pragma once

#include "app.hpp"

/* Precompute the mock-work sampler from the config (once, before launch). */
void responder_init(const AppConfig &cfg, uint64_t tsc_hz);
/* Worker entry point for Role::Responder (rte_eal_remote_launch). */
int responder_loop(void *arg);
