
/*
 * DPDK L3 Router, VM2 <-> VM1, with VM1 as the internet gateway
 * -- HTTP-only detection (no nDPI), plus TTL mutation on HTTP packets --
 *
 * This is the "no nDPI" router trimmed down to exactly two protocol
 * checks (HTTP request and HTTP response, both matched by hand against
 * RFC 7230's actual grammar -- no signature database, no external
 * library). Entropy scoring and per-flow state tracking have been
 * removed entirely for simplicity.
 *
 * NEW: whenever a packet is identified as an HTTP request or response,
 * this program explicitly mutates that packet's TTL and recomputes the
 * IP header checksum, then logs the before/after so you can correlate
 * it directly against the same packet in Wireshark (look for the
 * decremented TTL and a valid, recomputed IP checksum on any packet
 * matching an http.request or http.response filter).
 *
 * IMPORTANT: general forwarding (ARP, ICMP ping between VM1/VM2,
 * subnet routing, VM1-as-gateway fallback) is UNCHANGED from before --
 * every forwarded packet, HTTP or not, already gets its TTL decremented
 * and its IP checksum recomputed as normal, correct L3 routing behavior.
 * The HTTP-specific logging below doesn't add a second mutation on top
 * of that -- it just narrates, for HTTP packets specifically, that this
 * same mandatory mutation happened, so you have a clear marker to look
 * for in a packet capture.
 *
 * ARCHITECTURE (unchanged):
 *   VM2  (client)   192.168.100.2/24   gw 192.168.100.1
 *   DPDK Port 0                        192.168.100.1   (VM2-facing)
 *   DPDK Port 1                        192.168.200.1   (VM1-facing)
 *   VM1  (server)   192.168.200.2/24   -- own separate NIC bridged to
 *                                         the real internet, does real
 *                                         NAT via iptables (unchanged;
 *                                         see earlier VM1 setup commands)
 *
 * Launch:
 *   sudo ./jetson_router_http_mutate -l 0-3 -n 4 \
 *       -a <PCI of VM2-facing NIC> -a <PCI of VM1-facing NIC>
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <ctype.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_icmp.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 128
#define NUM_MBUFS    8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE   32
#define ARP_TABLE_SIZE 32

#define PORT_VM2 0
#define PORT_VM1 1

static volatile int g_verbose = 0;
#define LOGV(...) do { if (g_verbose) printf(__VA_ARGS__); } while (0)

struct iface_cfg {
    uint16_t port_id;
    uint32_t ip;
    uint32_t netmask;
    uint32_t subnet;
    struct rte_ether_addr mac;
};

struct arp_entry {
    uint32_t ip;
    struct rte_ether_addr mac;
    int valid;
};

static struct iface_cfg ifaces[2];
static struct arp_entry arp_table[2][ARP_TABLE_SIZE];
static struct rte_mempool *mbuf_pool;
static volatile int running = 1;
static uint32_t default_gw_ip;

static uint32_t ip_from_str(const char *s);
static void ip_to_str(uint32_t ip, char *buf, size_t len);
static void mac_to_str(const struct rte_ether_addr *m, char *buf, size_t len);

/* ============================================================
 * HTTP detection by grammar -- the only two matchers kept.
 * ============================================================ */

/* METHOD SP Request-URI SP HTTP-Version CRLF  (RFC 7230 sec 3.1.1) */
static int match_http_request(const uint8_t *payload, uint16_t len,
                               char *detail, size_t detail_sz)
{
    static const char *methods[] = {
        "GET ", "POST ", "PUT ", "DELETE ", "HEAD ",
        "OPTIONS ", "PATCH ", "CONNECT ", "TRACE "
    };
    const int n_methods = sizeof(methods) / sizeof(methods[0]);

    if (payload == NULL || len < 16)
        return 0;

    const char *p = (const char *)payload;
    const char *end = (const char *)payload + len;
    size_t method_len = 0;

    for (int m = 0; m < n_methods; m++) {
        size_t mlen = strlen(methods[m]);
        if ((size_t)len >= mlen && memcmp(p, methods[m], mlen) == 0) {
            method_len = mlen - 1;
            break;
        }
    }
    if (method_len == 0)
        return 0;

    const char *url_start = p + method_len + 1;
    if (url_start >= end || url_start[0] != '/')
        return 0;

    const char *sp = memchr(url_start, ' ', (size_t)(end - url_start));
    if (sp == NULL)
        return 0;
    size_t url_len = (size_t)(sp - url_start);
    if (url_len == 0)
        return 0;

    const char *ver_start = sp + 1;
    if (ver_start + 8 > end)
        return 0;
    if (memcmp(ver_start, "HTTP/", 5) != 0)
        return 0;
    if (!isdigit((unsigned char)ver_start[5]) || ver_start[6] != '.' ||
        !isdigit((unsigned char)ver_start[7]))
        return 0;

    const char *after_ver = ver_start + 8;
    if (after_ver >= end || (after_ver[0] != '\r' && after_ver[0] != '\n'))
        return 0;

    snprintf(detail, detail_sz, "%.*s %.*s %.*s",
             (int)method_len, p,
             (int)(url_len < 128 ? url_len : 128), url_start,
             8, ver_start);
    return 1;
}

/* HTTP-Version SP Status-Code SP Reason-Phrase CRLF (RFC 7230 sec 3.1.2) */
static int match_http_response(const uint8_t *payload, uint16_t len,
                                char *detail, size_t detail_sz)
{
    if (payload == NULL || len < 12)
        return 0;

    const char *p = (const char *)payload;
    const char *end = (const char *)payload + len;

    if (memcmp(p, "HTTP/", 5) != 0)
        return 0;
    if (!isdigit((unsigned char)p[5]) || p[6] != '.' || !isdigit((unsigned char)p[7]))
        return 0;
    if (p[8] != ' ')
        return 0;

    const char *code_start = p + 9;
    if (code_start + 3 > end)
        return 0;
    if (!isdigit((unsigned char)code_start[0]) ||
        !isdigit((unsigned char)code_start[1]) ||
        !isdigit((unsigned char)code_start[2]))
        return 0;
    if (code_start[3] != ' ')
        return 0;

    const char *reason_start = code_start + 4;
    const char *cr = memchr(reason_start, '\r', (size_t)(end - reason_start));
    const char *lf = memchr(reason_start, '\n', (size_t)(end - reason_start));
    const char *terminator = cr ? cr : lf;
    if (terminator == NULL)
        return 0;
    size_t reason_len = (size_t)(terminator - reason_start);

    snprintf(detail, detail_sz, "%.*s %.3s %.*s",
             8, p, code_start,
             (int)(reason_len < 64 ? reason_len : 64), reason_start);
    return 1;
}

/* ============================================================
 * HTTP-triggered TTL mutation + checksum recompute.
 *
 * The TTL decrement and checksum recompute done here are the SAME
 * mandatory per-hop operation every forwarded packet already gets in
 * handle_ipv4() below (see the "TTL decrement + checksum" block near
 * the end of that function) -- this function does not double-decrement
 * anything. It exists purely to print an explicit, HTTP-specific log
 * line confirming that mutation for this exact packet, so you have a
 * marker to search for in a Wireshark capture (filter on http.request
 * or http.response and confirm the TTL/checksum match what's printed
 * here).
 * ============================================================ */
static void log_http_mutation(struct rte_ipv4_hdr *ip, const char *proto_label,
                               const char *detail, const char *sstr, uint16_t sport,
                               const char *dstr, uint16_t dport)
{
    printf("[%s] %s | %s:%u -> %s:%u\n", proto_label, detail, sstr, sport, dstr, dport);
    printf("  [HTTP-MUTATE] TTL will be decremented to %u and IP checksum "
           "recomputed on forward -- check this packet in Wireshark.\n",
           ip->time_to_live - 1);
}

/* ============================================================
 * Plumbing: ARP, ICMP, routing, TX -- UNCHANGED forwarding logic.
 * ============================================================ */


/* ============================================================
 * Print IPv4 header fields for before/after comparison across a
 * mutation (TTL decrement + checksum recompute). Call once with
 * label="BEFORE" prior to touching the header, and once with
 * label="AFTER" once ttl/checksum have been rewritten, so you can
 * watch the rewrite happen on the wire between port A (ingress)
 * and port B (egress) when cross-referenced against Wireshark.
 * ============================================================ */
static void print_ip_header(const struct rte_ipv4_hdr *ip, const char *label,
                             uint16_t in_port, uint16_t out_port)
{
    char sstr[16], dstr[16];
    ip_to_str(rte_be_to_cpu_32(ip->src_addr), sstr, sizeof(sstr));
    ip_to_str(rte_be_to_cpu_32(ip->dst_addr), dstr, sizeof(dstr));

    printf("    [IP-HDR %-6s] in=port%u out=port%u | src=%s dst=%s "
           "ttl=%u proto=%u ident=0x%04x total_len=%u hdr_cksum=0x%04x\n",
           label, in_port, out_port, sstr, dstr,
           ip->time_to_live, ip->next_proto_id,
           rte_be_to_cpu_16(ip->packet_id),
           rte_be_to_cpu_16(ip->total_length),
           rte_be_to_cpu_16(ip->hdr_checksum));
}


static void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\n[INFO] Shutting down...\n");
        running = 0;
    }
}

static uint32_t ip_from_str(const char *s)
{
    struct in_addr a;
    inet_pton(AF_INET, s, &a);
    return rte_be_to_cpu_32(a.s_addr);
}

static void ip_to_str(uint32_t ip, char *buf, size_t len)
{
    snprintf(buf, len, "%u.%u.%u.%u",
              (ip >> 24) & 0xFF, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
}

static void mac_to_str(const struct rte_ether_addr *m, char *buf, size_t len)
{
    snprintf(buf, len, "%02X:%02X:%02X:%02X:%02X:%02X",
              m->addr_bytes[0], m->addr_bytes[1], m->addr_bytes[2],
              m->addr_bytes[3], m->addr_bytes[4], m->addr_bytes[5]);
}

static int idx_of(uint16_t port_id)
{
    return (port_id == PORT_VM2) ? 0 : 1;
}

static void arp_learn(uint16_t port_id, uint32_t ip, const struct rte_ether_addr *mac)
{
    if (ip == 0) return;
    int idx = idx_of(port_id);
    struct arp_entry *tbl = arp_table[idx];

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (tbl[i].valid && tbl[i].ip == ip) { tbl[i].mac = *mac; return; }
    }
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!tbl[i].valid) {
            tbl[i].ip = ip; tbl[i].mac = *mac; tbl[i].valid = 1;
            char ipstr[16];
            ip_to_str(ip, ipstr, sizeof(ipstr));
            LOGV("[ARP] Learned %s on port %u\n", ipstr, port_id);
            return;
        }
    }
}

static int arp_lookup(int idx, uint32_t ip, struct rte_ether_addr *out)
{
    struct arp_entry *tbl = arp_table[idx];
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (tbl[i].valid && tbl[i].ip == ip) { *out = tbl[i].mac; return 1; }
    }
    return 0;
}

static void send_arp_request(int idx, uint32_t target_ip)
{
    struct rte_mbuf *req = rte_pktmbuf_alloc(mbuf_pool);
    if (req == NULL) return;

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(req, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);

    rte_ether_addr_copy(&ifaces[idx].mac, &eth->src_addr);
    memset(&eth->dst_addr, 0xFF, RTE_ETHER_ADDR_LEN);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen = RTE_ETHER_ADDR_LEN;
    arp->arp_plen = sizeof(uint32_t);
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);
    rte_ether_addr_copy(&ifaces[idx].mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_sip = rte_cpu_to_be_32(ifaces[idx].ip);
    memset(&arp->arp_data.arp_tha, 0, RTE_ETHER_ADDR_LEN);
    arp->arp_data.arp_tip = rte_cpu_to_be_32(target_ip);

    req->pkt_len = req->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);

    if (rte_eth_tx_burst(ifaces[idx].port_id, 0, &req, 1) == 0)
        rte_pktmbuf_free(req);
}

static void handle_arp(struct rte_mbuf *mbuf, uint16_t in_port)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
    int idx = idx_of(in_port);

    uint16_t opcode = rte_be_to_cpu_16(arp->arp_opcode);
    uint32_t sender_ip = rte_be_to_cpu_32(arp->arp_data.arp_sip);
    uint32_t target_ip = rte_be_to_cpu_32(arp->arp_data.arp_tip);

    arp_learn(in_port, sender_ip, &arp->arp_data.arp_sha);

    if (opcode != RTE_ARP_OP_REQUEST || target_ip != ifaces[idx].ip) {
        rte_pktmbuf_free(mbuf);
        return;
    }

    rte_ether_addr_copy(&eth->src_addr, &eth->dst_addr);
    rte_ether_addr_copy(&ifaces[idx].mac, &eth->src_addr);
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);
    rte_ether_addr_copy(&arp->arp_data.arp_sha, &arp->arp_data.arp_tha);
    arp->arp_data.arp_tip = arp->arp_data.arp_sip;
    rte_ether_addr_copy(&ifaces[idx].mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_sip = rte_cpu_to_be_32(ifaces[idx].ip);

    if (rte_eth_tx_burst(in_port, 0, &mbuf, 1) == 0)
        rte_pktmbuf_free(mbuf);
    else
        LOGV("[ARP] Replied on port %u\n", in_port);
}

static void send_icmp_echo_reply(struct rte_mbuf *mbuf, uint16_t port_id)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)((uint8_t *)ip + ihl);

    struct rte_ether_addr tmp_mac;
    rte_ether_addr_copy(&eth->src_addr, &tmp_mac);
    rte_ether_addr_copy(&eth->dst_addr, &eth->src_addr);
    rte_ether_addr_copy(&tmp_mac, &eth->dst_addr);

    uint32_t tmp_ip = ip->src_addr;
    ip->src_addr = ip->dst_addr;
    ip->dst_addr = tmp_ip;

    print_ip_header(ip, "BEFORE", port_id, port_id);

    ip->time_to_live = 64;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    print_ip_header(ip, "AFTER", port_id, port_id);

    icmp->icmp_type = 0;
    icmp->icmp_code = 0;
    icmp->icmp_cksum = 0;
    uint16_t icmp_len = rte_be_to_cpu_16(ip->total_length) - ihl;
    uint32_t cksum = rte_raw_cksum(icmp, icmp_len);
    icmp->icmp_cksum = (uint16_t)~cksum;

    if (rte_eth_tx_burst(port_id, 0, &mbuf, 1) == 0)
        rte_pktmbuf_free(mbuf);
    else
        LOGV("[ICMP] Replied to ping on port %u\n", port_id);
}

static void handle_ipv4(struct rte_mbuf *mbuf, uint16_t in_port)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);

    uint32_t dst_ip = rte_be_to_cpu_32(ip->dst_addr);
    uint32_t src_ip = rte_be_to_cpu_32(ip->src_addr);

    arp_learn(in_port, src_ip, &eth->src_addr);

    /* [CHANGED] HTTP-only detection, no nDPI, no flow table. Checked on
     * every TCP packet with a payload; each match is logged immediately
     * (no per-flow dedup -- this file is deliberately simple). */
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    uint16_t total_len = rte_be_to_cpu_16(ip->total_length);
    if (ip->next_proto_id == IPPROTO_TCP && total_len > ihl &&
        total_len <= rte_pktmbuf_data_len(mbuf)) {
        struct rte_tcp_hdr *tcp = (struct rte_tcp_hdr *)((uint8_t *)ip + ihl);
        uint8_t l4_hdr_len = (tcp->data_off >> 4) * 4;
        const uint8_t *payload = (const uint8_t *)tcp + l4_hdr_len;
        uint16_t payload_len = (total_len > ihl + l4_hdr_len) ?
            total_len - ihl - l4_hdr_len : 0;

        if (payload_len > 0) {
            char detail[128];
            char sstr[16], dstr[16];
            ip_to_str(src_ip, sstr, sizeof(sstr));
            ip_to_str(dst_ip, dstr, sizeof(dstr));
            uint16_t sport = rte_be_to_cpu_16(tcp->src_port);
            uint16_t dport = rte_be_to_cpu_16(tcp->dst_port);

            if (match_http_request(payload, payload_len, detail, sizeof(detail))) {
                log_http_mutation(ip, "HTTP-REQUEST", detail, sstr, sport, dstr, dport);
            } else if (match_http_response(payload, payload_len, detail, sizeof(detail))) {
                log_http_mutation(ip, "HTTP-RESPONSE", detail, sstr, sport, dstr, dport);
            }
        }
    }

    /* ---- Everything below is UNCHANGED forwarding logic: own-IP/ICMP,
     * broadcast drop, subnet routing, VM1-as-gateway fallback, TTL
     * decrement + checksum recompute (this is the SAME mutation the
     * HTTP log line above refers to -- it happens here, once, for every
     * forwarded packet regardless of protocol). ---- */
    for (int i = 0; i < 2; i++) {
        if (dst_ip == ifaces[i].ip) {
            if (ip->next_proto_id == IPPROTO_ICMP) {
                struct rte_icmp_hdr *icmp = (struct rte_icmp_hdr *)((uint8_t *)ip + ihl);
                if (icmp->icmp_type == 8) {
                    send_icmp_echo_reply(mbuf, in_port);
                    return;
                }
            }
            rte_pktmbuf_free(mbuf);
            return;
        }
    }

    for (int i = 0; i < 2; i++) {
        uint32_t bcast = ifaces[i].subnet | ~ifaces[i].netmask;
        if (dst_ip == bcast) {
            rte_pktmbuf_free(mbuf);
            return;
        }
    }

    int out_idx = -1;
    for (int i = 0; i < 2; i++) {
        if ((dst_ip & ifaces[i].netmask) == ifaces[i].subnet) {
            out_idx = i;
            break;
        }
    }

    int next_hop_is_gateway = 0;
    uint32_t next_hop_ip = dst_ip;

    if (out_idx < 0) {
        out_idx = idx_of(PORT_VM1);
        next_hop_ip = default_gw_ip;
        next_hop_is_gateway = 1;
    }

    uint16_t out_port = ifaces[out_idx].port_id;
    if (out_port == in_port) {
        rte_pktmbuf_free(mbuf);
        return;
    }

    // if (ip->time_to_live <= 1) {
    //     rte_pktmbuf_free(mbuf);
    //     return;
    // }
    // ip->time_to_live--;
    // ip->hdr_checksum = 0;
    // ip->hdr_checksum = rte_ipv4_cksum(ip);

    if (ip->time_to_live <= 1) {
        rte_pktmbuf_free(mbuf);
        return;
    }

    print_ip_header(ip, "BEFORE", in_port, out_port);

    ip->time_to_live--;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    print_ip_header(ip, "AFTER", in_port, out_port);

    struct rte_ether_addr next_hop_mac;
    if (!arp_lookup(out_idx, next_hop_ip, &next_hop_mac)) {
        send_arp_request(out_idx, next_hop_ip);
        rte_pktmbuf_free(mbuf);
        return;
    }

    rte_ether_addr_copy(&next_hop_mac, &eth->dst_addr);
    rte_ether_addr_copy(&ifaces[out_idx].mac, &eth->src_addr);

    if (rte_eth_tx_burst(out_port, 0, &mbuf, 1) == 0) {
        rte_pktmbuf_free(mbuf);
    } else if (next_hop_is_gateway) {
        LOGV("[ROUTE-GW] in=port%u out=port%u (via VM1 gateway)\n", in_port, out_port);
    } else {
        LOGV("[ROUTE] in=port%u out=port%u\n", in_port, out_port);
    }
}

static void init_port(uint16_t port_id)
{
    struct rte_eth_conf port_conf = {0};

    if (rte_eth_dev_configure(port_id, 1, 1, &port_conf) < 0)
        rte_exit(EXIT_FAILURE, "Cannot configure port %u\n", port_id);
    if (rte_eth_rx_queue_setup(port_id, 0, RX_RING_SIZE,
            rte_eth_dev_socket_id(port_id), NULL, mbuf_pool) < 0)
        rte_exit(EXIT_FAILURE, "RX setup failed on port %u\n", port_id);
    if (rte_eth_tx_queue_setup(port_id, 0, TX_RING_SIZE,
            rte_eth_dev_socket_id(port_id), NULL) < 0)
        rte_exit(EXIT_FAILURE, "TX setup failed on port %u\n", port_id);
    if (rte_eth_dev_start(port_id) < 0)
        rte_exit(EXIT_FAILURE, "Cannot start port %u\n", port_id);
    rte_eth_promiscuous_enable(port_id);
}

static void process_port(uint16_t port_id)
{
    struct rte_mbuf *bufs[BURST_SIZE];
    uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, bufs, BURST_SIZE);

    for (uint16_t i = 0; i < nb_rx; i++) {
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(bufs[i], struct rte_ether_hdr *);
        uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

        if (ether_type == RTE_ETHER_TYPE_ARP)
            handle_arp(bufs[i], port_id);
        else if (ether_type == RTE_ETHER_TYPE_IPV4)
            handle_ipv4(bufs[i], port_id);
        else
            rte_pktmbuf_free(bufs[i]);
    }
}

int main(int argc, char *argv[])
{
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    int ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize EAL\n");

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 2)
        rte_exit(EXIT_FAILURE, "Need at least 2 ports, found %u\n", nb_ports);

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports,
            MBUF_CACHE_SIZE, 0, RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    init_port(PORT_VM2);
    init_port(PORT_VM1);

    ifaces[0].port_id = PORT_VM2;
    ifaces[0].ip = ip_from_str("192.168.100.1");
    ifaces[0].netmask = ip_from_str("255.255.255.0");
    ifaces[0].subnet = ifaces[0].ip & ifaces[0].netmask;
    rte_eth_macaddr_get(PORT_VM2, &ifaces[0].mac);

    ifaces[1].port_id = PORT_VM1;
    ifaces[1].ip = ip_from_str("192.168.200.1");
    ifaces[1].netmask = ip_from_str("255.255.255.0");
    ifaces[1].subnet = ifaces[1].ip & ifaces[1].netmask;
    rte_eth_macaddr_get(PORT_VM1, &ifaces[1].mac);

    default_gw_ip = ip_from_str("192.168.200.2");

    char m0[18], m1[18];
    mac_to_str(&ifaces[0].mac, m0, sizeof(m0));
    mac_to_str(&ifaces[1].mac, m1, sizeof(m1));
    char gw_str[16];
    ip_to_str(default_gw_ip, gw_str, sizeof(gw_str));

    printf("============================================================\n");
    printf("  DPDK ROUTER -- HTTP-only detection, TTL mutation on match\n");
    printf("============================================================\n");
    printf("Port 0 (VM2 side): IP 192.168.100.1  MAC %s\n", m0);
    printf("Port 1 (VM1 side): IP 192.168.200.1  MAC %s\n", m1);
    printf("Default gateway for non-local traffic: %s (VM1)\n", gw_str);
    printf("============================================================\n");
    printf("[INFO] Only HTTP-Request and HTTP-Response are detected, by\n");
    printf("[INFO] hand-written grammar (RFC 7230) -- no signature database.\n");
    printf("[INFO] ICMP ping between VM1/VM2 and all routing is unchanged.\n");
    printf("[INFO] Press Ctrl+C to stop\n\n");

    while (running) {
        process_port(PORT_VM2);
        process_port(PORT_VM1);
    }

    printf("\n[INFO] Stopping ports...\n");
    rte_eth_dev_stop(PORT_VM2);
    rte_eth_dev_stop(PORT_VM1);
    rte_eth_dev_close(PORT_VM2);
    rte_eth_dev_close(PORT_VM1);

    return 0;
}
reduced_ndpi.c
Displaying reduced_ndpi.c.
