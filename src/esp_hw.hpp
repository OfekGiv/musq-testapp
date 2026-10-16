/* Hardware ESP encapsulation for the requester (--esp-encap hw).
 *
 * The lcores send plain IPv4/UDP frames and the NIC turns each one into an
 * IPsec ESP transport-mode packet on its way out: it inserts the ESP header
 * (SPI and a sequence number it increments itself, one counter per SA), the
 * IV, the trailer and the ICV, encrypts the payload (AES-GCM) and fixes the
 * IPv4 protocol, length and checksum. This is the NIC's IPsec packet offload,
 * programmed through DOCA Flow: one SA for the whole port, applied by an egress
 * rule to every IPv4 packet any lcore sends, so there is one sequence number
 * stream over all lcores together, in the order the packets leave. (The lcores
 * keep their own source IPs; those only steer the responses.)
 *
 * DOCA Flow owns the port's steering once it is started, so the responses are
 * steered to their owner's queue by a DOCA ingress rule on the destination IP
 * instead of the rte_flow rule of the software mode. The responses stay
 * encrypted: nothing here decrypts.
 *
 * Needs a build with -Desp_hw=enabled (system DOCA Flow); otherwise the calls
 * below fail with a message.
 */
#pragma once

#include "app.hpp"

/* ESP transport mode with AES-GCM as the NIC builds it: SPI + SN, an 8 B IV,
 * the payload padded to 4 B together with the 2 B trailer (pad length, next
 * header), and a 16 B ICV. */
constexpr uint16_t ESP_HW_HDR_LEN = 8 + 8;
constexpr uint16_t ESP_HW_ICV_LEN = 16;
constexpr uint16_t ESP_HW_TRAILER_LEN = 2;
constexpr uint32_t ESP_HW_SPI = 0x00001000; /* the SPI the software mode writes */

struct EspHwLen {
    uint16_t plain_content; /* frame the lcore builds, without FCS */
    uint16_t wire_content;  /* the same frame after the NIC's encapsulation, without FCS */
};

/* The largest plain frame whose encapsulated form fits `frame_size` on the wire
 * (incl. FCS). The encapsulated size moves in steps of 4 B (ESP padding), so
 * the wire frame is frame_size or up to 3 B shorter. */
static inline EspHwLen esp_hw_lengths(uint16_t frame_size)
{
    const uint16_t l2l3 = 14 + 20;
    const uint16_t fixed = static_cast<uint16_t>(l2l3 + ESP_HW_HDR_LEN + ESP_HW_ICV_LEN);
    const uint16_t content = static_cast<uint16_t>(frame_size - 4);
    if (content < fixed + 4)
        return {0, 0};
    const uint16_t enc = static_cast<uint16_t>((content - fixed) & ~3u); /* payload + padding + trailer */
    return {static_cast<uint16_t>(l2l3 + enc - ESP_HW_TRAILER_LEN), static_cast<uint16_t>(fixed + enc)};
}

/* True when this binary was built with -Desp_hw=enabled. */
bool esp_hw_available();
/* After rte_eal_init: open the PF as a DOCA device and probe its ethdev (EAL
 * itself must not have probed it). 0 on success. */
int esp_hw_probe(const AppConfig &cfg);
/* After the port is started: start DOCA Flow on it, create the SA and the
 * egress rule, and steer each worker's responses (destination = its source IP)
 * to its RX queue. src_ips[i] is worker i's source IP (network order). 0 on
 * success. */
int esp_hw_start(AppState &st, const rte_be32_t *src_ips);
/* Packets the egress rules have encapsulated so far (hardware counters). */
uint64_t esp_hw_encap_pkts();
/* Before the port is stopped: remove the rules and stop the DOCA Flow port. */
void esp_hw_stop();
/* After the port is closed: release DOCA Flow and the device. */
void esp_hw_cleanup();
