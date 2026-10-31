/* Mock per-packet work: a TSC busy spin whose length is drawn from the
 * configured distribution. Header-only so the responder loop inlines it. */
#pragma once

#include "app.hpp"

#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <rte_udp.h>
#include <rte_pause.h>

#include <cmath>

static inline uint64_t xorshift64s(uint64_t &s)
{
    s ^= s >> 12;
    s ^= s << 25;
    s ^= s >> 27;
    return s * 0x2545F4914F6CDD1DULL;
}

struct WorkSampler {
    static constexpr unsigned EXP_TABLE_BITS = 10;
    static constexpr unsigned EXP_TABLE_SIZE = 1u << EXP_TABLE_BITS;

    WorkKind kind = WorkKind::Fixed;
    uint64_t lo_cycles = 0;
    uint64_t hi_cycles = 0;
    uint32_t hi_threshold = 0;         /* bimodal: PRNG (32-bit) < threshold -> hi */
    uint64_t cycles_per_us = 0;        /* header mode */
    uint64_t exp_table[EXP_TABLE_SIZE] = {};

    void init(const WorkSpec &spec, uint64_t tsc_hz)
    {
        kind = spec.kind;
        const auto ns_to_cyc = [tsc_hz](uint64_t ns) -> uint64_t {
            return static_cast<uint64_t>((static_cast<unsigned __int128>(ns) * tsc_hz) / 1000000000ULL);
        };
        lo_cycles = ns_to_cyc(spec.lo_ns);
        hi_cycles = ns_to_cyc(spec.hi_ns);
        hi_threshold = static_cast<uint32_t>((static_cast<uint64_t>(spec.pct_hi) * 0xFFFFFFFFULL) / 100ULL);
        cycles_per_us = tsc_hz / 1000000ULL;
        if (kind == WorkKind::Exp) {
            /* Quantile table of an exponential with the given mean, sampled at
             * the midpoints of EXP_TABLE_SIZE equiprobable bins: no log() on
             * the fast path, and the sample mean matches the requested mean. */
            const double mean = static_cast<double>(spec.lo_ns);
            for (unsigned i = 0; i < EXP_TABLE_SIZE; i++) {
                const double u = (static_cast<double>(i) + 0.5) / static_cast<double>(EXP_TABLE_SIZE);
                exp_table[i] = ns_to_cyc(static_cast<uint64_t>(-mean * std::log(u)));
            }
        }
    }

    bool enabled() const { return kind != WorkKind::Fixed || lo_cycles != 0; }
};

/* Locate the korba request header in an IPv4/UDP or IPv4/ESP (SPI+SN, then the
 * header) frame of len bytes at data; nullptr otherwise. *udp_out is set when
 * the packet is UDP so the caller can zero the UDP checksum after modifying the
 * payload. */
static inline request_header *find_request_header_data(uint8_t *data, uint16_t len, struct rte_udp_hdr **udp_out)
{
    *udp_out = nullptr;
    auto *eth = reinterpret_cast<struct rte_ether_hdr *>(data);
    if (eth->ether_type != rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4))
        return nullptr;
    auto *ip = reinterpret_cast<struct rte_ipv4_hdr *>(eth + 1);
    if ((ip->version_ihl & 0x0f) != 5)
        return nullptr;
    unsigned off = sizeof(*eth) + sizeof(*ip);
    if (ip->next_proto_id == IPPROTO_UDP) {
        *udp_out = reinterpret_cast<struct rte_udp_hdr *>(ip + 1);
        off += sizeof(struct rte_udp_hdr);
    } else if (ip->next_proto_id == 50) {
        off += 8;
    } else {
        return nullptr;
    }
    if (len < off + sizeof(request_header))
        return nullptr;
    auto *hdr = reinterpret_cast<request_header *>(data + off);
    return hdr->magic_number == REQUEST_MAGIC ? hdr : nullptr;
}

static inline request_header *find_request_header(struct rte_mbuf *m, struct rte_udp_hdr **udp_out)
{
    return find_request_header_data(rte_pktmbuf_mtod(m, uint8_t *), m->data_len, udp_out);
}

/* Cycles of mock work for a packet (its data and length are only read in
 * header mode). */
static inline uint64_t sample_cycles_data(const WorkSampler &ws, uint64_t &prng, uint8_t *data, uint16_t len)
{
    switch (ws.kind) {
    case WorkKind::Fixed:
        return ws.lo_cycles;
    case WorkKind::Bimodal:
        return static_cast<uint32_t>(xorshift64s(prng) >> 32) < ws.hi_threshold ? ws.hi_cycles : ws.lo_cycles;
    case WorkKind::Exp:
        return ws.exp_table[xorshift64s(prng) >> (64 - WorkSampler::EXP_TABLE_BITS)];
    case WorkKind::Header: {
        struct rte_udp_hdr *udp;
        const request_header *h = find_request_header_data(data, len, &udp);
        return h ? static_cast<uint64_t>(h->processing_time_us) * ws.cycles_per_us : 0;
    }
    }
    return 0;
}

static inline uint64_t sample_cycles(const WorkSampler &ws, uint64_t &prng, struct rte_mbuf *m)
{
    if (ws.kind != WorkKind::Header)
        return sample_cycles_data(ws, prng, nullptr, 0);
    return sample_cycles_data(ws, prng, rte_pktmbuf_mtod(m, uint8_t *), m->data_len);
}

/* CPU-bound spin (l3fwd l3fwd_busy_wait_ns), in TSC cycles. */
static inline void busy_wait_cycles(uint64_t cycles)
{
    if (cycles == 0)
        return;
    const uint64_t start = rte_rdtsc();
    while (rte_rdtsc() - start < cycles)
        rte_pause();
}
