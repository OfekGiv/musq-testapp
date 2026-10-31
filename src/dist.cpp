#include "dist.hpp"
#include "config.hpp"
#include "forward.hpp"
#include "lane_epoch.hpp"

#include <rte_cycles.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_prefetch.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

/* ── tokens ────────────────────────────────────────────────────────────── */

/* What a worker gets for a packet: the frame address (48 bits on x86-64) and
 * its length in the top 16 bits. The mbuf itself stays with the master. */
constexpr uint64_t TOK_ADDR_MASK = (1ULL << 48) - 1;

static inline void *make_tok(const struct rte_mbuf *m)
{
    const uint64_t addr = reinterpret_cast<uintptr_t>(rte_pktmbuf_mtod(m, const uint8_t *));
    return reinterpret_cast<void *>(static_cast<uintptr_t>(addr | (static_cast<uint64_t>(m->data_len) << 48)));
}

static inline uint8_t *tok_data(const void *t)
{
    return reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(t) & TOK_ADDR_MASK));
}

static inline uint16_t tok_len(const void *t)
{
    return static_cast<uint16_t>(reinterpret_cast<uintptr_t>(t) >> 48);
}

/* ── set-up and tear-down ──────────────────────────────────────────────── */

int dist_init(AppState &st)
{
    const AppConfig &cfg = st.cfg;
    if (!cfg.dist())
        return 0;
    const unsigned wpm = cfg.workers_per_master();
    const bool seqn = cfg.dist_order == DistOrder::Seqn;
    int master_ctx[RTE_MAX_LCORE];  /* context index of each master, by master index */
    unsigned linked[RTE_MAX_LCORE]; /* workers linked so far, by master index */
    for (unsigned m = 0; m < RTE_MAX_LCORE; m++) {
        master_ctx[m] = -1;
        linked[m] = 0;
    }

    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        if (w.dist_role != DistRole::Master)
            continue;
        const int socket = static_cast<int>(rte_lcore_to_socket_id(w.lcore_id));
        auto *dm = static_cast<DistMaster *>(rte_zmalloc_socket("dist_master", sizeof(DistMaster),
                                                                 RTE_CACHE_LINE_SIZE, socket));
        if (dm == nullptr) {
            APP_LOG_ERR("cannot allocate the distributor state of lcore %u", w.lcore_id);
            return -1;
        }
        dm->nb_workers = static_cast<uint16_t>(wpm);
        dm->nb_slots = static_cast<uint16_t>(wpm + (cfg.dist_self ? 1 : 0));
        dm->chunk = static_cast<uint16_t>(cfg.dist_chunk);
        dm->chunk_left = dm->chunk;
        dm->cchunk_left = dm->chunk;
        dm->self_len = static_cast<uint16_t>(cfg.dist_self);
        dm->self_backlog = cfg.dist_self_backlog;
        dm->cap = cfg.dist_ring - cfg.rx_burst;
        dm->ring_mask = cfg.dist_ring - 1;
        dm->rx_lcore = dm->tx_lcore = w.lcore_id;
        if (seqn) {
            const uint32_t rob = dist_rob_size(cfg);
            dm->rob = static_cast<struct rte_mbuf **>(rte_zmalloc_socket("dist_rob", sizeof(*dm->rob) * rob,
                                                                          RTE_CACHE_LINE_SIZE, socket));
            if (dm->rob == nullptr) {
                APP_LOG_ERR("cannot allocate the reorder buffer (%u entries) of lcore %u", rob, w.lcore_id);
                return -1;
            }
            dm->rob_mask = rob - 1;
        }
        if (cfg.dist_self) {
            /* The self slot: the master's own FIFO (no rings), and with rr
             * the per-cycle lengths for the collector. */
            DistLinkRx &R = dm->rx[wpm];
            DistLinkTx &T = dm->tx[wpm];
            R.mbuf_fifo = static_cast<struct rte_mbuf **>(rte_zmalloc_socket(
                "dist_self_mbufs", sizeof(*R.mbuf_fifo) * cfg.dist_ring, RTE_CACHE_LINE_SIZE, socket));
            dm->self_fifo = static_cast<uint16_t *>(rte_zmalloc_socket("dist_self_fifo", sizeof(uint16_t) * cfg.dist_ring,
                                                                         RTE_CACHE_LINE_SIZE, socket));
            if (R.mbuf_fifo == nullptr || dm->self_fifo == nullptr) {
                APP_LOG_ERR("cannot allocate the self slot of lcore %u", w.lcore_id);
                return -1;
            }
            if (seqn) {
                R.seq_fifo = static_cast<uint32_t *>(rte_zmalloc_socket("dist_self_seq", sizeof(uint32_t) * cfg.dist_ring,
                                                                         RTE_CACHE_LINE_SIZE, socket));
                if (R.seq_fifo == nullptr) {
                    APP_LOG_ERR("cannot allocate the self slot's sequence FIFO of lcore %u", w.lcore_id);
                    return -1;
                }
            }
            dm->self_fifo_mask = cfg.dist_ring - 1;
            T.mbuf_fifo = R.mbuf_fifo;
            T.seq_fifo = R.seq_fifo;
            T.lcore_id = w.lcore_id;
        }
        w.dm = dm;
        master_ctx[w.master_idx] = static_cast<int>(i);
    }

    /* --tx-masters: the collecting half shares its master's state. */
    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        if (w.dist_role != DistRole::TxMaster)
            continue;
        if (w.master_idx >= cfg.nb_masters() || master_ctx[w.master_idx] < 0) {
            APP_LOG_ERR("lcore %u: no master with index %u", w.lcore_id, w.master_idx);
            return -1;
        }
        DistMaster *dm = st.workers[master_ctx[w.master_idx]].dm;
        dm->tx_lcore = w.lcore_id;
        w.dm = dm;
    }

    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        if (w.dist_role != DistRole::Worker)
            continue;
        if (w.master_idx >= cfg.nb_masters() || master_ctx[w.master_idx] < 0) {
            APP_LOG_ERR("lcore %u: no master with index %u", w.lcore_id, w.master_idx);
            return -1;
        }
        WorkerCtx &mw = st.workers[master_ctx[w.master_idx]];
        DistMaster *dm = mw.dm;
        const unsigned k = linked[w.master_idx]++;
        if (k >= dm->nb_workers) {
            APP_LOG_ERR("lcore %u: master lcore %u already has %u workers", w.lcore_id, mw.lcore_id, dm->nb_workers);
            return -1;
        }
        DistLinkRx &R = dm->rx[k];
        DistLinkTx &T = dm->tx[k];
        /* The rings and FIFOs live on the master's socket; a worker of another
         * socket pays on every access (the socket warning at start-up names it). */
        const int socket = static_cast<int>(rte_lcore_to_socket_id(mw.lcore_id));
        char in_name[RTE_RING_NAMESIZE], out_name[RTE_RING_NAMESIZE];
        snprintf(in_name, sizeof(in_name), "dist_in_%u", w.lcore_id);
        snprintf(out_name, sizeof(out_name), "dist_out_%u", w.lcore_id);
        R.in = rte_ring_create(in_name, cfg.dist_ring, socket, RING_F_SP_ENQ | RING_F_SC_DEQ);
        T.out = rte_ring_create(out_name, cfg.dist_ring, socket, RING_F_SP_ENQ | RING_F_SC_DEQ);
        if (R.in == nullptr || T.out == nullptr) {
            APP_LOG_ERR("cannot create the rings of lcore %u (%u entries): %s", w.lcore_id, cfg.dist_ring,
                        rte_strerror(rte_errno));
            return -1;
        }
        T.lcore_id = w.lcore_id;
        R.mbuf_fifo = static_cast<struct rte_mbuf **>(rte_zmalloc_socket(
            "dist_mbufs", sizeof(*R.mbuf_fifo) * cfg.dist_ring, RTE_CACHE_LINE_SIZE, socket));
        if (R.mbuf_fifo == nullptr) {
            APP_LOG_ERR("cannot allocate the mbuf FIFO of lcore %u", w.lcore_id);
            return -1;
        }
        if (seqn) {
            R.seq_fifo = static_cast<uint32_t *>(rte_zmalloc_socket("dist_seq", sizeof(uint32_t) * cfg.dist_ring,
                                                                     RTE_CACHE_LINE_SIZE, socket));
            if (R.seq_fifo == nullptr) {
                APP_LOG_ERR("cannot allocate the sequence FIFO of lcore %u", w.lcore_id);
                return -1;
            }
        }
        T.mbuf_fifo = R.mbuf_fifo;
        T.seq_fifo = R.seq_fifo;
        auto *dw = static_cast<DistWorker *>(rte_zmalloc_socket(
            "dist_worker", sizeof(DistWorker), RTE_CACHE_LINE_SIZE, static_cast<int>(rte_lcore_to_socket_id(w.lcore_id))));
        if (dw == nullptr) {
            APP_LOG_ERR("cannot allocate the worker state of lcore %u", w.lcore_id);
            return -1;
        }
        dw->in = R.in;
        dw->out = T.out;
        dw->master_lcore = mw.lcore_id;
        w.dw = dw;
        APP_LOG_INFO("lcore %u: software worker %u of master lcore %u, rings %s and %s (%u entries each, "
                     "at most %u packets in flight)",
                     w.lcore_id, k, mw.lcore_id, in_name, out_name, cfg.dist_ring, dm->cap);
    }

    for (unsigned i = 0; i < st.nb_workers; i++) {
        const WorkerCtx &w = st.workers[i];
        if (w.dist_role != DistRole::Master)
            continue;
        const DistMaster *dm = w.dm;
        if (linked[w.master_idx] != dm->nb_workers) {
            APP_LOG_ERR("master lcore %u has %u workers, expected %u", w.lcore_id, linked[w.master_idx], dm->nb_workers);
            return -1;
        }
        if (cfg.dist_split() && dm->tx_lcore == dm->rx_lcore) {
            APP_LOG_ERR("master lcore %u has no TX master", w.lcore_id);
            return -1;
        }
        std::string lcores;
        for (unsigned k = 0; k < dm->nb_workers; k++)
            lcores += (k ? "," : "") + std::to_string(dm->tx[k].lcore_id);
        APP_LOG_INFO("lcore %u: master of %u workers (lcores %s): order %s, deal %s, chunk %u, rings of %u, "
                     "reorder buffer %u, self %u (backlog %u), rx queue(s) %u..%u, tx queue %u%s%s",
                     w.lcore_id, dm->nb_workers, lcores.c_str(), dist_order_name(cfg.dist_order),
                     dist_deal_name(cfg.dist_deal), dm->chunk, cfg.dist_ring, seqn ? dm->rob_mask + 1u : 0u,
                     dm->self_len, dm->self_backlog, w.queue_id, w.queue_id + w.nb_rx_queues - 1, w.master_idx,
                     cfg.dist_split() ? " collected and sent by TX master lcore " : "",
                     cfg.dist_split() ? std::to_string(dm->tx_lcore).c_str() : "");
    }
    return 0;
}

void dist_cleanup(AppState &st)
{
    if (!st.cfg.dist())
        return;
    for (unsigned i = 0; i < st.nb_workers; i++) {
        WorkerCtx &w = st.workers[i];
        if (w.dw != nullptr) {
            rte_free(w.dw);
            w.dw = nullptr;
        }
        if (w.dist_role == DistRole::TxMaster) {
            w.dm = nullptr; /* its master's, freed below */
            continue;
        }
        DistMaster *dm = w.dm;
        if (dm == nullptr)
            continue;
        for (unsigned k = 0; k < dm->nb_slots; k++) {
            DistLinkRx &R = dm->rx[k];
            DistLinkTx &T = dm->tx[k];
            /* Packets dealt and not yet sent are still in the FIFO (the rings
             * only hold tokens; the self slot has none); with seqn the
             * collected ones sit in the reorder buffer, freed below. */
            if (R.mbuf_fifo != nullptr)
                for (uint64_t c = T.collected; c != R.dealt; c++)
                    rte_pktmbuf_free(R.mbuf_fifo[c & dm->ring_mask]);
            rte_ring_free(R.in);
            rte_ring_free(T.out);
            R.in = T.out = nullptr;
            rte_free(R.mbuf_fifo);
            rte_free(R.seq_fifo);
            R.mbuf_fifo = T.mbuf_fifo = nullptr;
            R.seq_fifo = T.seq_fifo = nullptr;
        }
        if (dm->rob != nullptr) {
            for (uint32_t j = 0; j <= dm->rob_mask; j++)
                if (dm->rob[j] != nullptr)
                    rte_pktmbuf_free(dm->rob[j]);
            rte_free(dm->rob);
        }
        rte_free(dm->self_fifo);
        rte_free(dm);
        w.dm = nullptr;
    }
}

/* ── ring helpers ──────────────────────────────────────────────────────── */

/* An enqueue cannot fail by construction (dist.hpp): the master never has more
 * packets in flight per worker than either ring holds. Should one fail anyway
 * it is counted and retried; a drop would break the order. */
static inline void ring_put(struct rte_ring *r, void *const *objs, unsigned n, WorkerStats &s)
{
    while (unlikely(rte_ring_sp_enqueue_bulk(r, objs, n, nullptr) == 0)) {
        s.ring_retries++;
        rte_pause();
    }
}

static inline unsigned ring_get(struct rte_ring *r, void **objs, unsigned n)
{
    return rte_ring_sc_dequeue_burst(r, objs, n, nullptr);
}

/* ── deal side ─────────────────────────────────────────────────────────── */

/* Packets at a worker as far as the dealer knows: dealt and not yet seen
 * taken back, including those of the RX burst being dealt right now. Never
 * less than the truth, so a decision on it is safe. */
static inline uint64_t in_flight(const DistLinkRx &R)
{
    return R.dealt + R.deal_len - R.collected_seen;
}

/* Read what the collector has published: its collected count per worker and,
 * with seqn, the number of the next packet to send. Acquire against the
 * collector's release stores, so the FIFO entries it read before publishing
 * may be overwritten after this. Without --tx-masters these are the lcore's
 * own fields. */
static inline void refresh_seen(DistMaster *dm)
{
    for (uint16_t k = 0; k < dm->nb_slots; k++)
        dm->rx[k].collected_seen = __atomic_load_n(&dm->tx[k].collected, __ATOMIC_ACQUIRE);
    dm->head_seq_seen = __atomic_load_n(&dm->head_seq, __ATOMIC_ACQUIRE);
}

/* --dist-self: whether the dealer keeps the next self slot: it had idle
 * passes lately (self_ok, from the loop's own accounting) and some worker
 * has self_backlog or more tokens waiting in its input ring, i.e. is behind
 * (not merely uncollected: a worker's returns waiting for the collector are
 * not its backlog). The ring count reads each worker's consumer index, once
 * per cycle. */
static inline bool decide_self(DistMaster *dm)
{
    if (!dm->self_ok)
        return false;
    unsigned mx = 0;
    for (uint16_t k = 0; k < dm->nb_workers; k++)
        mx = RTE_MAX(mx, rte_ring_count(dm->rx[k].in));
    return mx >= dm->self_backlog;
}

static inline uint16_t least_loaded(const DistMaster *dm)
{
    uint16_t best = 0;
    uint64_t best_n = UINT64_MAX;
    for (uint16_t k = 0; k < dm->nb_workers; k++) {
        const uint64_t n = in_flight(dm->rx[k]);
        if (n < best_n) {
            best_n = n;
            best = k;
        }
    }
    return best;
}

/* Whether a whole RX burst can be dealt without any ring filling up, by what
 * the dealer last saw: round robin may give a worker the whole burst, so
 * every worker needs room; lb loads the least loaded worker first, so that
 * one does. With sequence numbers the reorder buffer must have room for the
 * burst as well. */
template <DistOrder ORDER, bool SELF>
static inline bool room_by_seen(const DistMaster *dm, bool lb, uint16_t rx_burst)
{
    uint64_t ext = lb ? UINT64_MAX : 0;
    for (uint16_t k = 0; k < dm->nb_workers; k++) {
        const uint64_t n = in_flight(dm->rx[k]);
        ext = lb ? RTE_MIN(ext, n) : RTE_MAX(ext, n);
    }
    /* The self slot's FIFO must have room for the burst as well. */
    if constexpr (SELF)
        ext = RTE_MAX(ext, in_flight(dm->rx[dm->nb_workers]));
    if (ext + rx_burst > dm->cap)
        return false;
    if (ORDER == DistOrder::Seqn && (dm->next_seq - dm->head_seq_seen) + rx_burst > dm->rob_mask + 1u)
        return false;
    return true;
}

/* The same, re-reading the collector's counters only when the last-seen
 * values say no: the dealer touches the collector's cache lines only when a
 * worker is near the limit. */
template <DistOrder ORDER, bool SELF>
static inline bool room_for_burst(DistMaster *dm, bool lb, uint16_t rx_burst)
{
    if (likely((room_by_seen<ORDER, SELF>(dm, lb, rx_burst))))
        return true;
    refresh_seen(dm);
    return room_by_seen<ORDER, SELF>(dm, lb, rx_burst);
}

/* Loop invariants of the master's own processing (--dist-self). */
struct SelfWork {
    bool work_on = false;
    bool header_work = false;
    SwapMode swap = SwapMode::L2L3;
};

/* --dist-self, rr: move the deal cursor to the next slot of the cycle. On
 * entering the self slot the dealer decides its length for this cycle and
 * records it for the collector; a length of 0 skips the slot. */
static __rte_always_inline void self_advance(DistMaster *dm, uint16_t &cur, uint16_t &left)
{
    const uint16_t self = dm->nb_workers, nb_slots = dm->nb_slots;
    for (;;) {
        cur = static_cast<uint16_t>(cur + 1 == nb_slots ? 0 : cur + 1);
        if (cur != self) {
            left = dm->chunk;
            return;
        }
        left = decide_self(dm) ? dm->self_len : 0;
        dm->self_fifo[dm->self_cycle_d++ & dm->self_fifo_mask] = left;
        if (left != 0)
            return;
    }
}

/* Deal one RX burst: each packet's mbuf (and number) into the FIFO of its
 * slot and its token into that worker's deal buffer (the chunk cursor runs
 * on across bursts), then one bulk enqueue per worker. With SELF the master
 * is one more slot: its packets are processed here, straight from its FIFO,
 * and their count published. With rr the cursor moves on as soon as a chunk
 * is complete, so the self slot's length for a cycle is on record before the
 * preceding chunk's tokens are released to the worker (the collector can
 * only reach the self slot after those). Without SELF this is the plain
 * dealer, unchanged. */
template <DistOrder ORDER, bool SELF>
static __rte_always_inline void deal_burst(WorkerCtx *w, DistMaster *dm, struct rte_mbuf **pkts, uint16_t n, bool lb,
                                           const SelfWork &sw)
{
    WorkerStats &s = w->stats;
    DistLinkRx *links = dm->rx;
    const uint16_t nb_w = dm->nb_workers, chunk = dm->chunk;
    const uint32_t fmask = dm->ring_mask;
    uint16_t cur = dm->cursor, left = dm->chunk_left;
    uint32_t seq = dm->next_seq;
    if constexpr (!SELF) {
        for (uint16_t i = 0; i < n; i++) {
            if (lb && left == chunk) /* a chunk starts: pick its worker */
                cur = least_loaded(dm);
            DistLinkRx &R = links[cur];
            const uint32_t idx = static_cast<uint32_t>(R.dealt + R.deal_len) & fmask;
            R.mbuf_fifo[idx] = pkts[i];
            if (ORDER == DistOrder::Seqn)
                R.seq_fifo[idx] = seq++;
            R.tok[R.deal_len++] = make_tok(pkts[i]);
            if (--left == 0) {
                left = chunk;
                cur = static_cast<uint16_t>(cur + 1 == nb_w ? 0 : cur + 1);
            }
        }
    } else {
        /* The same loop; the self slot gets tokens too (unused) so that the
         * per-packet work is identical, and only the chunk end differs. */
        for (uint16_t i = 0; i < n; i++) {
            DistLinkRx &R = links[cur];
            const uint32_t idx = static_cast<uint32_t>(R.dealt + R.deal_len) & fmask;
            R.mbuf_fifo[idx] = pkts[i];
            if (ORDER == DistOrder::Seqn)
                R.seq_fifo[idx] = seq++;
            R.tok[R.deal_len++] = make_tok(pkts[i]);
            if (--left == 0) { /* a chunk ends: the next slot, on record before the releases below */
                if (!lb) {
                    self_advance(dm, cur, left);
                } else if (decide_self(dm)) {
                    cur = nb_w;
                    left = dm->self_len;
                } else {
                    cur = least_loaded(dm);
                    left = chunk;
                }
            }
        }
    }
    dm->cursor = cur;
    dm->chunk_left = left;
    dm->next_seq = seq;
    for (uint16_t k = 0; k < nb_w; k++) {
        DistLinkRx &R = links[k];
        if (R.deal_len == 0)
            continue;
        ring_put(R.in, R.tok, R.deal_len, s);
        R.dealt += R.deal_len;
        s.dealt += R.deal_len;
        R.deal_len = 0;
    }
    if constexpr (SELF) {
        /* The workers have theirs; now the master's own share, in place in
         * its FIFO (at most two runs round the wrap), then published to the
         * collector (release: the frames are done). */
        DistLinkRx &R = links[nb_w];
        if (R.deal_len != 0) {
            const uint32_t start = static_cast<uint32_t>(R.dealt) & fmask;
            const uint16_t first = static_cast<uint16_t>(std::min<uint32_t>(R.deal_len, fmask + 1 - start));
            process_burst(w, R.mbuf_fifo + start, first, sw.work_on, sw.header_work, sw.swap);
            if (first < R.deal_len)
                process_burst(w, R.mbuf_fifo, static_cast<uint16_t>(R.deal_len - first), sw.work_on, sw.header_work,
                              sw.swap);
            __atomic_store_n(&R.dealt, R.dealt + R.deal_len, __ATOMIC_RELEASE);
            s.self_pkts += R.deal_len;
            R.deal_len = 0;
        }
    }
}

/* ── collect side ──────────────────────────────────────────────────────── */

/* rr: whether a worker other than the cursor's has handed packets back that
 * wait behind it (head-of-line blocking by the cursor's worker). Only the
 * collector's own staging counts are looked at: no ring is read for it. */
static inline bool others_waiting(const DistMaster *dm, uint16_t cur)
{
    for (uint16_t k = 0; k < dm->nb_workers; k++)
        if (k != cur && dm->tx[k].returned != 0)
            return true;
    return false;
}

/* rr: take the packets back in the sequence they were dealt, into the TX
 * buffer, sending every full tx_burst. A worker's returned tokens are only
 * counted; the mbufs come from its FIFO. Stops at the first worker whose next
 * packet has not come back (and resumes there next pass), or after MAX_BURST
 * packets so the RX side is served. */
/* --dist-self, rr: the collector's next slot; the self slot has the length
 * the dealer recorded for this cycle, and is skipped when that is 0. */
static __rte_always_inline void self_next_slot(DistMaster *dm, uint16_t &cur, uint16_t &left)
{
    const uint16_t self = dm->nb_workers, nb_slots = dm->nb_slots;
    for (;;) {
        cur = static_cast<uint16_t>(cur + 1 == nb_slots ? 0 : cur + 1);
        if (cur != self) {
            left = dm->chunk;
            return;
        }
        left = dm->self_fifo[dm->self_cycle_c++ & dm->self_fifo_mask];
        if (left != 0)
            return;
    }
}

template <bool SELF>
static __rte_always_inline uint32_t collect_rr(WorkerCtx *w, DistMaster *dm, WorkerCtx::TxBuf &tb, uint16_t tx_burst)
{
    WorkerStats &s = w->stats;
    const uint16_t nb_w = dm->nb_workers, chunk = dm->chunk;
    const uint32_t fmask = dm->ring_mask;
    uint16_t cur = dm->ccursor, left = dm->cchunk_left;
    uint32_t total = 0;
    void *scratch[MAX_BURST];
    while (total < MAX_BURST) {
        DistLinkTx &T = dm->tx[cur];
        if (T.returned == 0) {
            unsigned n;
            if (SELF && cur == nb_w) /* the master's own packets: done as far as the dealer published */
                n = static_cast<unsigned>(std::min<uint64_t>(
                    __atomic_load_n(&dm->rx[nb_w].dealt, __ATOMIC_ACQUIRE) - T.collected, MAX_BURST));
            else
                n = ring_get(T.out, scratch, MAX_BURST);
            if (n == 0) {
                if (others_waiting(dm, cur))
                    s.stalls++;
                break;
            }
            T.returned = static_cast<uint16_t>(n);
        }
        uint16_t take = RTE_MIN(T.returned, left);
        take = RTE_MIN(take, static_cast<uint16_t>(tx_burst - tb.len));
        const uint64_t c = T.collected;
        struct rte_mbuf **fifo = T.mbuf_fifo;
        for (uint16_t k = 0; k < take; k++)
            tb.m[tb.len + k] = fifo[static_cast<uint32_t>(c + k) & fmask];
        tb.len = static_cast<uint16_t>(tb.len + take);
        T.returned = static_cast<uint16_t>(T.returned - take);
        /* Release: the FIFO entries just read may be overwritten by the
         * dealer once it sees this. */
        __atomic_store_n(&T.collected, c + take, __ATOMIC_RELEASE);
        left = static_cast<uint16_t>(left - take);
        total += take;
        if (tb.len == tx_burst)
            flush_txbuf(w, tb);
        if (left == 0) {
            if constexpr (SELF) {
                self_next_slot(dm, cur, left);
            } else {
                left = chunk;
                cur = static_cast<uint16_t>(cur + 1 == nb_w ? 0 : cur + 1);
            }
        }
    }
    dm->ccursor = cur;
    dm->cchunk_left = left;
    s.collected += total;
    return total;
}

/* seqn: everything the workers have handed back goes into the reorder buffer
 * by number, then the buffer is sent from its head as far as it is filled. */
template <bool SELF>
static __rte_always_inline uint32_t collect_seqn(WorkerCtx *w, DistMaster *dm, WorkerCtx::TxBuf &tb, uint16_t tx_burst)
{
    WorkerStats &s = w->stats;
    struct rte_mbuf **rob = dm->rob;
    const uint32_t rmask = dm->rob_mask, fmask = dm->ring_mask;
    uint32_t fill = dm->rob_fill;
    void *scratch[MAX_BURST];
    for (uint16_t k = 0; k < dm->nb_slots; k++) {
        DistLinkTx &T = dm->tx[k];
        unsigned n;
        if (SELF && k == dm->nb_workers) { /* the self slot: what the dealer published */
            const uint64_t d = __atomic_load_n(&dm->rx[k].dealt, __ATOMIC_ACQUIRE);
            n = static_cast<unsigned>(std::min<uint64_t>(d - T.collected, MAX_BURST));
        } else {
            n = ring_get(T.out, scratch, MAX_BURST);
        }
        if (n == 0)
            continue;
        uint64_t c = T.collected;
        for (unsigned i = 0; i < n; i++, c++) {
            const uint32_t idx = static_cast<uint32_t>(c) & fmask;
            rob[T.seq_fifo[idx] & rmask] = T.mbuf_fifo[idx];
        }
        __atomic_store_n(&T.collected, c, __ATOMIC_RELEASE);
        fill += n;
    }
    uint32_t head = dm->head_seq, total = 0;
    while (total < MAX_BURST) {
        struct rte_mbuf *m = rob[head & rmask];
        if (m == nullptr)
            break;
        rob[head & rmask] = nullptr;
        head++;
        tb.m[tb.len++] = m;
        total++;
        if (tb.len == tx_burst)
            flush_txbuf(w, tb);
    }
    fill -= total;
    if (total == 0 && fill != 0)
        s.stalls++; /* packets back, the head's not among them */
    dm->rob_fill = fill;
    /* Release: the dealer may number packets into the slots just emptied. */
    __atomic_store_n(&dm->head_seq, head, __ATOMIC_RELEASE);
    s.collected += total;
    return total;
}

/* ── master loops ──────────────────────────────────────────────────────── */

/* One lcore's share of a master: both halves (RX = TX = true, the master
 * without --tx-masters), the dealing half (the RX master) or the collecting
 * half (its TX master). */
template <DistOrder ORDER, bool RX, bool TX, bool SELF>
static int master_loop(AppState *st, WorkerCtx *w)
{
    WorkerStats &s = w->stats;
    const AppConfig &cfg = st->cfg;
    DistMaster *dm = w->dm;
    const unsigned lcore_id = w->lcore_id;
    const uint16_t port = w->port_id;
    const uint16_t rxq0 = w->queue_id, rxq_end = static_cast<uint16_t>(rxq0 + w->nb_rx_queues);
    const bool multi_rxq = w->nb_rx_queues > 1;
    uint16_t rxq = rxq0;
    const uint16_t rx_burst = cfg.rx_burst, tx_burst = cfg.tx_burst;
    const bool accumulate = cfg.tx_mode == TxMode::Accumulate;
    const bool lb = cfg.dist_deal == DistDeal::Lb;
    const uint64_t drain_tsc = (st->tsc_hz + 999999ULL) / 1000000ULL * cfg.tx_drain_us;
    WorkerCtx::TxBuf &tb = w->txbuf[0];
    struct rte_mbuf *pkts[MAX_BURST];
    SelfWork sw;
    sw.work_on = g_sampler.enabled();
    sw.header_work = g_sampler.kind == WorkKind::Header;
    sw.swap = cfg.swap;

    /* Keep the "entering main loop on lcore" wording: harnesses wait for it. */
    if (RX && TX)
        APP_LOG_INFO("entering main loop on lcore %u (worker %u, port %u, queue %u; master of %u workers, self %u)",
                     lcore_id, w->worker_idx, port, rxq0, dm->nb_workers, dm->self_len);
    else if (RX)
        APP_LOG_INFO("entering main loop on lcore %u (worker %u, port %u, rx queue %u; RX master of %u workers, "
                     "TX master lcore %u)", lcore_id, w->worker_idx, port, rxq0, dm->nb_workers, dm->tx_lcore);
    else
        APP_LOG_INFO("entering main loop on lcore %u (worker %u, port %u, tx queue %u; TX master of %u workers, "
                     "RX master lcore %u)", lcore_id, w->worker_idx, port, tb.queue_id, dm->nb_workers, dm->rx_lcore);

    uint64_t prev_drain = rte_rdtsc();
    s.loop_start_tsc = prev_drain;
    uint64_t t_prev = prev_drain;
    bool prev_busy = false;
    /* --dist-self: idle share of the last window of passes (SELF && RX). */
    const unsigned self_idle_pct = cfg.dist_self_idle;
    uint64_t win_used = s.cycles_used, win_wasted = s.cycles_wasted;
    uint32_t win_passes = 0;

    while (!st->force_quit) {
        /* One TSC read per pass, as in responder_loop: busy if the pass
         * received or collected anything. */
        const uint64_t t0 = rte_rdtsc();
        if (prev_busy)
            s.cycles_used += t0 - t_prev;
        else
            s.cycles_wasted += t0 - t_prev;
        t_prev = t0;

        if constexpr (SELF && RX) {
            if (unlikely(++win_passes == 1024)) {
                const uint64_t used = s.cycles_used - win_used, wasted = s.cycles_wasted - win_wasted;
                dm->self_ok = wasted * 100 >= (used + wasted) * self_idle_pct;
                win_used = s.cycles_used;
                win_wasted = s.cycles_wasted;
                win_passes = 0;
            }
        }

        uint32_t got = 0;
        uint16_t nb_rx = 0;

        if constexpr (TX) {
            lane_epoch_tick(w, t0);

            if (accumulate && unlikely(t0 - prev_drain > drain_tsc)) {
                flush_txbuf(w, tb);
                prev_drain = t0;
            }

            /* 1. What the workers handed back, in arrival order, into the TX
             *    buffer; every full tx_burst goes out at once. */
            got = ORDER == DistOrder::Rr ? collect_rr<SELF>(w, dm, tb, tx_burst) : collect_seqn<SELF>(w, dm, tb, tx_burst);
            if (!accumulate)
                flush_txbuf(w, tb);
            if constexpr (!RX)
                s.rx_pkts += got; /* a TX master's "RX" is what it collected */
        }

        if constexpr (RX) {
            /* 2. Receive and deal, if a whole burst fits. */
            if (lb)
                refresh_seen(dm); /* lb chooses by the current counts */
            if (likely((room_for_burst<ORDER, SELF>(dm, lb, rx_burst)))) {
                nb_rx = rte_eth_rx_burst(port, rxq, pkts, rx_burst);
                if (unlikely(multi_rxq))
                    rxq = static_cast<uint16_t>(rxq + 1 == rxq_end ? rxq0 : rxq + 1);
                if (nb_rx != 0) {
                    s.rx_bursts++;
                    s.rx_pkts += nb_rx;
                    if (nb_rx == rx_burst)
                        s.rx_full_bursts++;
                    deal_burst<ORDER, SELF>(w, dm, pkts, nb_rx, lb, sw);
                }
            } else {
                s.rx_held++;
            }
        }
        prev_busy = nb_rx != 0 || got != 0;
    }

    /* Do not transmit after the stop request: release what is buffered here;
     * what is still in flight is freed by dist_cleanup. */
    if constexpr (TX) {
        for (uint16_t i = 0; i < tb.len; i++)
            rte_pktmbuf_free(tb.m[i]);
        tb.len = 0;
    }
    APP_LOG_INFO("lcore %u leaving main loop: rx=%lu dealt=%lu self=%lu collected=%lu tx=%lu drops=%lu stalls=%lu "
                 "rx_held=%lu",
                 lcore_id, static_cast<unsigned long>(s.rx_pkts), static_cast<unsigned long>(s.dealt),
                 static_cast<unsigned long>(s.self_pkts), static_cast<unsigned long>(s.collected),
                 static_cast<unsigned long>(s.tx_pkts), static_cast<unsigned long>(s.tx_drops),
                 static_cast<unsigned long>(s.stalls), static_cast<unsigned long>(s.rx_held));
    return 0;
}

/* The order and the self slot are run-time choices; each combination is its
 * own instantiation so that the plain path carries nothing of the others. */
template <bool RX, bool TX>
static int run_master(AppState *st, WorkerCtx *w)
{
    const bool seqn = st->cfg.dist_order == DistOrder::Seqn, self = w->dm->self_len != 0;
    if (seqn)
        return self ? master_loop<DistOrder::Seqn, RX, TX, true>(st, w) : master_loop<DistOrder::Seqn, RX, TX, false>(st, w);
    return self ? master_loop<DistOrder::Rr, RX, TX, true>(st, w) : master_loop<DistOrder::Rr, RX, TX, false>(st, w);
}

int dist_master_loop(void *arg)
{
    AppState *st = static_cast<AppState *>(arg);
    const int widx = st->lcore_to_worker[rte_lcore_id()];
    if (widx < 0 || st->workers[widx].dm == nullptr || st->workers[widx].dist_role != DistRole::Master)
        return -1;
    WorkerCtx *w = &st->workers[widx];
    return st->cfg.dist_split() ? run_master<true, false>(st, w) : run_master<true, true>(st, w);
}

int dist_tx_master_loop(void *arg)
{
    AppState *st = static_cast<AppState *>(arg);
    const int widx = st->lcore_to_worker[rte_lcore_id()];
    if (widx < 0 || st->workers[widx].dm == nullptr || st->workers[widx].dist_role != DistRole::TxMaster)
        return -1;
    return run_master<false, true>(st, &st->workers[widx]);
}

/* ── worker ────────────────────────────────────────────────────────────── */

/* The responder's per-packet processing on tokens: header prefetch, mock work
 * (--work) and header swap (--swap); counters into w->stats. */
static __rte_always_inline void process_tokens(WorkerCtx *w, void *const *tok, uint16_t n, bool work_on,
                                               bool header_work, SwapMode swap)
{
    WorkerStats &s = w->stats;
    if (!work_on && swap == SwapMode::None)
        return; /* nothing to do: the frames go back untouched */
    const uint16_t ahead = RTE_MIN(HDR_PREFETCH, n);
    for (uint16_t i = 0; i < ahead; i++)
        rte_prefetch0(tok_data(tok[i]));
    if (!work_on) {
        uint64_t non_ipv4 = 0;
        for (uint16_t i = 0; i < n; i++) {
            if (i + HDR_PREFETCH < n)
                rte_prefetch0(tok_data(tok[i + HDR_PREFETCH]));
            non_ipv4 += !swap_headers_data(tok_data(tok[i]), swap);
        }
        s.non_ipv4 += non_ipv4;
        return;
    }
    for (uint16_t i = 0; i < n; i++) {
        if (i + HDR_PREFETCH < n)
            rte_prefetch0(tok_data(tok[i + HDR_PREFETCH]));
        uint8_t *data = tok_data(tok[i]);
        const uint16_t len = tok_len(tok[i]);

        request_header *rh = nullptr;
        struct rte_udp_hdr *rh_udp = nullptr;
        uint64_t cyc;
        if (header_work) {
            rh = find_request_header_data(data, len, &rh_udp);
            cyc = rh ? static_cast<uint64_t>(rh->processing_time_us) * g_sampler.cycles_per_us : 0;
        } else {
            cyc = sample_cycles_data(g_sampler, w->prng, data, len);
        }
        if (cyc) {
            busy_wait_cycles(cyc);
            s.work_cycles += cyc;
        }
        if (!swap_headers_data(data, swap))
            s.non_ipv4++;
        if (rh) {
            rh->magic_number = RESPONSE_MAGIC;
            /* Payload changed: a UDP checksum would be wrong now, zero = none. */
            if (rh_udp)
                rh_udp->dgram_cksum = 0;
        }
    }
}

int dist_worker_loop(void *arg)
{
    AppState *st = static_cast<AppState *>(arg);
    const unsigned lcore_id = rte_lcore_id();
    const int widx = st->lcore_to_worker[lcore_id];
    if (widx < 0 || st->workers[widx].dw == nullptr)
        return -1;
    WorkerCtx *w = &st->workers[widx];
    WorkerStats &s = w->stats;
    const AppConfig &cfg = st->cfg;
    struct rte_ring *in = w->dw->in, *out = w->dw->out;
    const uint16_t burst = cfg.rx_burst;
    const bool header_work = g_sampler.kind == WorkKind::Header;
    const bool work_on = g_sampler.enabled();
    const SwapMode swap = cfg.swap;
    void *tok[MAX_BURST];

    APP_LOG_INFO("entering main loop on lcore %u (worker %u, software worker of master lcore %u)",
                 lcore_id, w->worker_idx, w->dw->master_lcore);

    uint64_t t_prev = rte_rdtsc();
    s.loop_start_tsc = t_prev;
    bool prev_busy = false;

    while (!st->force_quit) {
        const uint64_t t0 = rte_rdtsc();
        if (prev_busy)
            s.cycles_used += t0 - t_prev;
        else
            s.cycles_wasted += t0 - t_prev;
        t_prev = t0;

        const uint16_t n = static_cast<uint16_t>(ring_get(in, tok, burst));
        prev_busy = n != 0;
        if (n == 0)
            continue;
        s.rx_bursts++;
        s.rx_pkts += n;
        if (n == burst)
            s.rx_full_bursts++;
        process_tokens(w, tok, n, work_on, header_work, swap);
        ring_put(out, tok, n, s);
        s.tx_pkts += n;
    }
    APP_LOG_INFO("lcore %u leaving main loop: processed=%lu", lcore_id, static_cast<unsigned long>(s.rx_pkts));
    return 0;
}
