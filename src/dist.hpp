/* Software distributor (--masters): the software alternative to mu_sq.
 *
 * A master lcore receives from its own RX queue(s), deals the packets to its
 * worker lcores over rings, takes them back, restores the arrival order and
 * sends them on its own TX queue. The workers only do the per-packet work
 * (--work, --swap). Two ways to restore the order (--dist-order):
 *
 *   rr    The master deals round robin, --dist-chunk consecutive packets per
 *         worker, and collects in exactly the same sequence (a collect cursor
 *         mirrors the deal cursor). The order is implicit: no sequence
 *         numbers. The analog of DOCA round-robin steering plus the mu_sq ring
 *         order; a slow worker holds the stream as a lagging lane does.
 *   seqn  The master numbers the packets. The number of every packet dealt to
 *         a worker goes into a master-local FIFO for that worker (workers hand
 *         packets back in the order they got them, so the k-th packet back has
 *         the k-th number); returns are placed in a reorder buffer by number
 *         and sent from its head. Workers may then be chosen round robin or by
 *         fewest packets in flight (--dist-deal lb).
 *
 * What crosses the rings is a token per packet: the address of the frame and
 * its length, not the mbuf. The mbuf pointers stay in a master-local FIFO per
 * worker (indexed by that worker's dealt / collected counters), so no worker
 * ever reads an mbuf and the mbuf cache lines are not shared between cores:
 * the master's RX and TX touch them as a run-to-completion core would. A
 * worker hands the tokens back in the order it got them; the master only
 * needs their count, the mbufs come from its FIFO.
 *
 * Flow control needs no shared counters: the master knows what it dealt to and
 * collected from each worker, and never has more than ring size - rx burst
 * packets in flight per worker, so neither ring can fill and no enqueue can
 * fail. When a worker is that far behind the master stops receiving (the NIC
 * RX ring absorbs and then drops, "RX missed") instead of dropping anything
 * itself, since a drop would break the order. The rings are the only memory a
 * master and a worker share. See README, "Software distributor".
 *
 * --tx-masters splits a master over two lcores, a two-stage pipeline: the
 * master (now the RX master) receives and deals, its TX master collects and
 * sends. The two halves share the per-worker FIFOs (the dealer writes an
 * entry before the token enters the ring, the collector reads it after the
 * token comes back out; the rings' release/acquire order carries the writes
 * across) and one counter per worker, the collector's collected count, which
 * the dealer reads only when its own copy of it says a worker is at the
 * limit (with seqn also the reorder buffer's head number). Every other field
 * is written by one half only and sits in that half's cache lines. The
 * mbufs themselves then do cross cores: the RX PMD writes them on the dealer,
 * the TX PMD reads and frees them on the collector, and they come back to the
 * dealer's pool cache through the mempool ring.
 */
#pragma once

#include "app.hpp"

#include <rte_ring.h>

/* The dealing lcore's part of a link to one worker: written by it only. */
struct alignas(RTE_CACHE_LINE_SIZE) DistLinkRx {
    struct rte_ring *in = nullptr;         /* dealer -> worker: tokens */
    /* Per packet dealt, indexed by dealt (write) and collected (read) modulo
     * the ring size: its mbuf, and with seqn its sequence number. */
    struct rte_mbuf **mbuf_fifo = nullptr;
    uint32_t *seq_fifo = nullptr;
    uint64_t dealt = 0;                    /* packets enqueued to in */
    uint64_t collected_seen = 0;           /* the collector's collected count as last read (refresh_seen) */
    uint16_t deal_len = 0;                 /* tokens of the RX burst being dealt that go to this worker */
    void *tok[MAX_BURST];
};

/* The collecting lcore's part: written by it only; collected is read by the
 * dealer (an acquire load against the collector's release store). */
struct alignas(RTE_CACHE_LINE_SIZE) DistLinkTx {
    struct rte_ring *out = nullptr;        /* worker -> collector: the same tokens, done */
    struct rte_mbuf **mbuf_fifo = nullptr; /* the same FIFOs as the dealer's */
    uint32_t *seq_fifo = nullptr;
    unsigned lcore_id = 0;                 /* the worker */
    uint64_t collected = 0;                /* rr: mbufs moved to the TX buffer; seqn: moved to the reorder buffer */
    uint16_t returned = 0;                 /* rr: tokens back from the worker whose mbufs are not yet in the TX buffer */
};

/* --dist-self: the master is one more slot in the deal cycle, after the
 * workers (link index nb_workers, no rings). When the dealer's cursor reaches
 * it, it keeps self_len packets of the stream for itself if some worker has
 * self_backlog or more tokens waiting in its input ring (it is behind) and
 * the dealer itself had idle passes lately (it has the cycles), else none; with rr order the
 * length it chose goes into self_fifo for the collector, which walks the same
 * cycle. The packets are processed by the dealer right after the burst is
 * dealt and their count published in the link's dealt (release), from which
 * the collector takes them as it would a worker's returns. */
struct alignas(RTE_CACHE_LINE_SIZE) DistMaster {
    /* Fixed after dist_init, read by both halves. */
    uint16_t nb_workers = 0;
    uint16_t nb_slots = 0;            /* nb_workers, + 1 with --dist-self */
    uint16_t chunk = 1;               /* --dist-chunk */
    uint16_t self_len = 0;            /* --dist-self, 0 = off */
    uint32_t self_backlog = 0;        /* --dist-self-backlog */
    uint16_t *self_fifo = nullptr;    /* rr: the self slot's length per cycle, written by the dealer, read by the collector */
    uint32_t self_fifo_mask = 0;
    uint32_t cap = 0;                 /* most packets in flight per worker: ring size - rx burst */
    uint32_t ring_mask = 0;           /* ring size - 1 */
    uint32_t rob_mask = 0;            /* seqn: reorder buffer size - 1 */
    unsigned rx_lcore = 0;            /* the dealer */
    unsigned tx_lcore = 0;            /* the collector (the same lcore without --tx-masters) */
    /* Deal side. */
    alignas(RTE_CACHE_LINE_SIZE) uint16_t cursor = 0; /* worker of the next packet */
    uint16_t chunk_left = 0;          /* packets left in its chunk */
    uint32_t next_seq = 0;            /* seqn: number of the next packet dealt */
    uint32_t head_seq_seen = 0;       /* seqn: head_seq as last read */
    uint64_t self_cycle_d = 0;        /* self slots entered by the dealer */
    bool self_ok = false;             /* the dealer had idle passes lately (--dist-self-idle), so it may take a share */
    /* Collect side. rr: the cursor mirrors the deal side's. */
    alignas(RTE_CACHE_LINE_SIZE) uint16_t ccursor = 0;
    uint16_t cchunk_left = 0;
    uint32_t head_seq = 0;            /* seqn: number of the next packet to send (read by the dealer) */
    uint32_t rob_fill = 0;            /* seqn: packets waiting in the reorder buffer */
    struct rte_mbuf **rob = nullptr;  /* seqn: reorder buffer, indexed by number & rob_mask */
    uint64_t self_cycle_c = 0;        /* self slots entered by the collector */
    DistLinkRx rx[MAX_DIST_WORKERS + 1]; /* [nb_workers] = the self slot */
    DistLinkTx tx[MAX_DIST_WORKERS + 1];
};

struct alignas(RTE_CACHE_LINE_SIZE) DistWorker {
    struct rte_ring *in = nullptr;
    struct rte_ring *out = nullptr;
    unsigned master_lcore = 0;
};

/* Reorder buffer entries of a master (seqn): a power of 2 that holds every
 * packet its workers can have in flight. */
static inline uint32_t dist_rob_size(const AppConfig &cfg)
{
    const uint64_t want = static_cast<uint64_t>(cfg.workers_per_master()) * cfg.dist_ring;
    uint32_t n = 1;
    while (n < want)
        n <<= 1;
    return n;
}

/* Create the rings and the per-lcore state. Call after the port is configured
 * and before the lcores are launched. 0, or -1 on error. */
int dist_init(AppState &st);
/* Free the packets still in flight, then the rings. Call after every lcore has
 * stopped and before the mempools go. */
void dist_cleanup(AppState &st);

int dist_master_loop(void *arg);     /* a master: both halves, or the RX half with --tx-masters */
int dist_tx_master_loop(void *arg);  /* a TX master (--tx-masters): the collecting half */
int dist_worker_loop(void *arg);
