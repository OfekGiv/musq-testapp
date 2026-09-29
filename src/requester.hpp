#pragma once

#include "app.hpp"

/* Phase 2: korba-style paced request generator. Each requester lcore paces
 * its own request stream (--rps per lcore), stamps every request with a
 * unique id and a service time (--service-time, carried in request_header as
 * microseconds), and polls its own RX queue for responses. A response may
 * arrive on any lcore (RSS), so completion is cross-lcore: the receiving lcore
 * looks the id up in the owner's tracker and records the RTT in its own
 * histogram. Lost requests are detected when a tracker slot is reused. */

/* Allocate trackers, histograms and packet templates for every worker. */
int requester_init(AppState &st);
/* Remove the response-steering flow rules; call before the port is stopped. */
void requester_stop(AppState &st);
void requester_cleanup(AppState &st);
/* Worker entry point for Role::Requester. */
int requester_loop(void *arg);
