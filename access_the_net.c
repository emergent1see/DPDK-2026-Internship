/*
 * dpdk_router.c
 *
 * Real Layer-3 DPDK router between two VMs on DIFFERENT subnets.
 *
 *      VM2 (192.168.100.2/24)                         VM1 (192.168.200.2/24)
 *            |                                                |
 *      enp0s8 (peer)                                    enp0s8 (peer)
 *            |                                                |
 *      Port 0 = 192.168.100.1 (gateway for VM2)   Port 1 = 192.168.200.1 (gateway for VM1)
 *            \_______________________ DPDK app ______________________/
 *
 * Unlike a pure L2 relay, this app:
 *   1. Owns an IP address on each port (acts as the default gateway for each VM).
 *   2. Answers ARP requests for its own gateway IPs.
 *   3. Learns each VM's real IP/MAC from ARP traffic (and from IP traffic too).
 *   4. Resolves the far-side VM's MAC before forwarding an IP packet (sending an
 *      ARP request and dropping the packet if not yet resolved -- exactly like a
 *      real router / Linux box would do on a cache miss).
 *   5. Decrements TTL and recomputes the IPv4 header checksum, because that's what
 *      routers do to every packet they forward (a pure L2 bridge does NOT do this).
 *   6. Rewrites Ethernet src MAC to the outgoing port's own MAC and dst MAC to the
 *      resolved next-hop's MAC -- this is mandatory once two separate subnets and a
 *      real routed hop are involved (a bridge doesn't need this; a router does).
 *
 *   7. [ADDED] Internet access: VM2 has a second, separate NIC with real internet
 *      access (NAT'd by the hypervisor) and does IP masquerading for VM1's subnet.
 *      Any destination that isn't 192.168.100.0/24 or 192.168.200.0/24 (e.g. Google's
 *      real IP) is forwarded out Port 0 toward VM2, since VM2 is this router's only
 *      path to the wider internet -- this is a "default route" in the same sense a
 *      home router's WAN side is its default route for unrecognized destinations.
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <arpa/inet.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_byteorder.h>
#include <rte_malloc.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 128
#define NUM_MBUFS 8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE 32

#define PORT_VM2 0   /* faces VM2, subnet 192.168.100.0/24 */
#define PORT_VM1 1   /* faces VM1, subnet 192.168.200.0/24 */

#define ARP_TABLE_SIZE 16

static volatile int running = 1;

/* ---- Per-port "router interface" configuration ------------------------- */
struct router_iface {
    uint16_t         port_id;
    uint32_t         ip;       /* host byte order */
    uint32_t         subnet;   /* network address, host byte order */
    uint32_t         netmask;  /* host byte order, e.g. 0xFFFFFF00 for /24 */
    struct rte_ether_addr mac;
    int              is_default_route; /* [ADDED] 1 = unmatched destinations go out this port */
    uint32_t         next_hop_ip;      /* [ADDED] real host to ARP for when acting as default route */
};

static struct router_iface ifaces[2];

/* ---- Tiny per-port ARP cache -------------------------------------------- */
struct arp_entry {
    uint32_t ip;                 /* host byte order, 0 = unused slot */
    struct rte_ether_addr mac;
    int valid;
};

static struct arp_entry arp_table[2][ARP_TABLE_SIZE];

/* ------------------------------------------------------------------------ */
static void signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\n[INFO] Shutting down...\n");
        running = 0;
    }
}

static void ip_to_str(uint32_t ip, char *buf, size_t size)
{
    snprintf(buf, size, "%u.%u.%u.%u",
             (ip >> 24) & 0xFF, (ip >> 16) & 0xFF,
             (ip >> 8) & 0xFF, ip & 0xFF);
}

static void mac_to_str(const struct rte_ether_addr *mac, char *buf, size_t size)
{
    snprintf(buf, size, "%02X:%02X:%02X:%02X:%02X:%02X",
              mac->addr_bytes[0], mac->addr_bytes[1], mac->addr_bytes[2],
              mac->addr_bytes[3], mac->addr_bytes[4], mac->addr_bytes[5]);
}

/* ---- ARP table helpers --------------------------------------------------- */

/* which local port_idx (0 or 1) owns the interface for this port_id */
static int port_idx(uint16_t port_id)
{
    return (port_id == PORT_VM2) ? 0 : 1;
}

static void arp_learn(uint16_t port_id, uint32_t ip, const struct rte_ether_addr *mac)
{
    int idx = port_idx(port_id);
    struct arp_entry *tbl = arp_table[idx];
    int free_slot = -1;

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (tbl[i].valid && tbl[i].ip == ip) {
            /* update existing entry (MAC may have changed) */
            if (!rte_is_same_ether_addr(&tbl[i].mac, mac)) {
                rte_ether_addr_copy(mac, &tbl[i].mac);
                char ipstr[16], macstr[18];
                ip_to_str(ip, ipstr, sizeof(ipstr));
                mac_to_str(mac, macstr, sizeof(macstr));
                printf("[ARP] Updated %s -> %s on port %u\n", ipstr, macstr, port_id);
            }
            return;
        }
        if (!tbl[i].valid && free_slot < 0)
            free_slot = i;
    }

    if (free_slot < 0)
        free_slot = 0; /* table full: overwrite slot 0 (fine for this simple use-case) */

    tbl[free_slot].ip = ip;
    rte_ether_addr_copy(mac, &tbl[free_slot].mac);
    tbl[free_slot].valid = 1;

    char ipstr[16], macstr[18];
    ip_to_str(ip, ipstr, sizeof(ipstr));
    mac_to_str(mac, macstr, sizeof(macstr));
    printf("[ARP] Learned %s -> %s on port %u\n", ipstr, macstr, port_id);
}

static int arp_lookup(uint16_t port_id, uint32_t ip, struct rte_ether_addr *out_mac)
{
    int idx = port_idx(port_id);
    struct arp_entry *tbl = arp_table[idx];

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (tbl[i].valid && tbl[i].ip == ip) {
            rte_ether_addr_copy(&tbl[i].mac, out_mac);
            return 1;
        }
    }
    return 0;
}

/* ---- ARP packet construction --------------------------------------------- */

/* Send an ARP request out `port_id` asking "who has target_ip" */
static void send_arp_request(struct rte_mempool *pool, uint16_t port_id, uint32_t target_ip)
{
    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(pool);
    if (!mbuf) return;

    struct router_iface *iface = &ifaces[port_idx(port_id)];

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);

    rte_ether_addr_copy(&iface->mac, &eth->src_addr);
    memset(&eth->dst_addr, 0xFF, RTE_ETHER_ADDR_LEN); /* broadcast */
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen = RTE_ETHER_ADDR_LEN;
    arp->arp_plen = 4;
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REQUEST);

    rte_ether_addr_copy(&iface->mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_sip = rte_cpu_to_be_32(iface->ip);
    memset(&arp->arp_data.arp_tha, 0, RTE_ETHER_ADDR_LEN);
    arp->arp_data.arp_tip = rte_cpu_to_be_32(target_ip);

    mbuf->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    mbuf->pkt_len = mbuf->data_len;

    rte_eth_tx_burst(port_id, 0, &mbuf, 1);

    char ipstr[16];
    ip_to_str(target_ip, ipstr, sizeof(ipstr));
    printf("[ARP] Sent request on port %u for %s\n", port_id, ipstr);
}

/* Reply to an ARP request that targeted one of our own interface IPs */
static void send_arp_reply(struct rte_mbuf *req_mbuf, uint16_t port_id)
{
    struct rte_ether_hdr *req_eth = rte_pktmbuf_mtod(req_mbuf, struct rte_ether_hdr *);
    struct rte_arp_hdr *req_arp = (struct rte_arp_hdr *)(req_eth + 1);
    struct router_iface *iface = &ifaces[port_idx(port_id)];

    struct rte_mbuf *mbuf = rte_pktmbuf_alloc(req_mbuf->pool);
    if (!mbuf) return;

    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);

    rte_ether_addr_copy(&iface->mac, &eth->src_addr);
    rte_ether_addr_copy(&req_eth->src_addr, &eth->dst_addr);
    eth->ether_type = rte_cpu_to_be_16(RTE_ETHER_TYPE_ARP);

    arp->arp_hardware = rte_cpu_to_be_16(RTE_ARP_HRD_ETHER);
    arp->arp_protocol = rte_cpu_to_be_16(RTE_ETHER_TYPE_IPV4);
    arp->arp_hlen = RTE_ETHER_ADDR_LEN;
    arp->arp_plen = 4;
    arp->arp_opcode = rte_cpu_to_be_16(RTE_ARP_OP_REPLY);

    rte_ether_addr_copy(&iface->mac, &arp->arp_data.arp_sha);
    arp->arp_data.arp_sip = rte_cpu_to_be_32(iface->ip); /* "I am the gateway" */
    rte_ether_addr_copy(&req_arp->arp_data.arp_sha, &arp->arp_data.arp_tha);
    arp->arp_data.arp_tip = req_arp->arp_data.arp_sip;

    mbuf->data_len = sizeof(struct rte_ether_hdr) + sizeof(struct rte_arp_hdr);
    mbuf->pkt_len = mbuf->data_len;

    rte_eth_tx_burst(port_id, 0, &mbuf, 1);

    char ipstr[16];
    ip_to_str(iface->ip, ipstr, sizeof(ipstr));
    printf("[ARP] Replied on port %u: I am %s\n", port_id, ipstr);
}

/* Handle any incoming ARP packet (request or reply) on the given port */
static void handle_arp(struct rte_mbuf *mbuf, uint16_t port_id)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    struct rte_arp_hdr *arp = (struct rte_arp_hdr *)(eth + 1);
    struct router_iface *iface = &ifaces[port_idx(port_id)];

    uint32_t sender_ip = rte_be_to_cpu_32(arp->arp_data.arp_sip);
    uint32_t target_ip = rte_be_to_cpu_32(arp->arp_data.arp_tip);
    uint16_t opcode = rte_be_to_cpu_16(arp->arp_opcode);

    /* Always learn the sender's IP/MAC -- this is how we discover VM1/VM2 */
    arp_learn(port_id, sender_ip, &arp->arp_data.arp_sha);

    if (opcode == RTE_ARP_OP_REQUEST && target_ip == iface->ip) {
        /* They're asking for OUR gateway IP -- answer it */
        send_arp_reply(mbuf, port_id);
    }

    rte_pktmbuf_free(mbuf);
}

/* ---- IPv4 forwarding ------------------------------------------------------ */

static void handle_ipv4(struct rte_mbuf *mbuf, uint16_t in_port, struct rte_mempool *pool)
{
    struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
    struct rte_ipv4_hdr *ip = (struct rte_ipv4_hdr *)(eth + 1);

    uint32_t dst_ip = rte_be_to_cpu_32(ip->dst_addr);
    uint32_t src_ip = rte_be_to_cpu_32(ip->src_addr);

    /* Learn the sender from plain IP traffic too (not just ARP) */
    arp_learn(in_port, src_ip, &eth->src_addr);

    /* Decide the outgoing port based on destination subnet.
     * [ADDED] If the destination matches neither known local subnet (e.g. it's
     * a real internet IP), fall through to whichever interface is marked as
     * the default route instead of dropping. */
    uint16_t out_port;
    struct router_iface *out_iface = NULL;
    uint32_t arp_target_ip; /* [ADDED] the IP we actually need a MAC for */

    if ((dst_ip & ifaces[0].netmask) == ifaces[0].subnet) {
        out_iface = &ifaces[0];
        arp_target_ip = dst_ip;               /* on-link: destination IS the next hop */
    } else if ((dst_ip & ifaces[1].netmask) == ifaces[1].subnet) {
        out_iface = &ifaces[1];
        arp_target_ip = dst_ip;               /* on-link: destination IS the next hop */
    } else {
        for (int i = 0; i < 2; i++) {
            if (ifaces[i].is_default_route) {
                out_iface = &ifaces[i];
                break;
            }
        }
        if (!out_iface) {
            char dstr[16];
            ip_to_str(dst_ip, dstr, sizeof(dstr));
            printf("[DROP] No route to %s (no default route configured)\n", dstr);
            rte_pktmbuf_free(mbuf);
            return;
        }
        /* Not on-link -- we can never ARP for e.g. google.com's IP directly.
         * Resolve the next-hop gateway (VM2's own IP) instead. */
        arp_target_ip = out_iface->next_hop_ip;
    }
    out_port = out_iface->port_id;

    /* Don't forward back out the interface it came in on */
    if (out_port == in_port) {
        rte_pktmbuf_free(mbuf);
        return;
    }

    /* Resolve next-hop MAC */
    struct rte_ether_addr next_hop_mac;
    if (!arp_lookup(out_port, arp_target_ip, &next_hop_mac)) {
        char hstr[16];
        ip_to_str(arp_target_ip, hstr, sizeof(hstr));
        printf("[ARP MISS] Don't know MAC for next-hop %s yet -- requesting, dropping this packet\n", hstr);
        send_arp_request(pool, out_port, arp_target_ip);
        rte_pktmbuf_free(mbuf);
        return;
    }

    /* Router behavior: decrement TTL, recompute checksum */
    if (ip->time_to_live <= 1) {
        printf("[DROP] TTL expired\n");
        rte_pktmbuf_free(mbuf);
        return;
    }
    ip->time_to_live--;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    /* Rewrite Ethernet header for the new hop */
    rte_ether_addr_copy(&out_iface->mac, &eth->src_addr);
    rte_ether_addr_copy(&next_hop_mac, &eth->dst_addr);

    char sstr[16], dstr[16];
    ip_to_str(src_ip, sstr, sizeof(sstr));
    ip_to_str(dst_ip, dstr, sizeof(dstr));
    const char *proto = (ip->next_proto_id == IPPROTO_ICMP) ? "ICMP" :
                         (ip->next_proto_id == IPPROTO_TCP)  ? "TCP"  :
                         (ip->next_proto_id == IPPROTO_UDP)  ? "UDP"  : "OTHER";
    printf("[ROUTE] %s: %s -> %s | in=port%u out=port%u\n", proto, sstr, dstr, in_port, out_port);

    rte_eth_tx_burst(out_port, 0, &mbuf, 1);
}

/* ---- Per-port packet dispatch --------------------------------------------- */

static void process_port(uint16_t port_id, struct rte_mempool *pool)
{
    struct rte_mbuf *bufs[BURST_SIZE];
    uint16_t nb_rx = rte_eth_rx_burst(port_id, 0, bufs, BURST_SIZE);

    for (uint16_t i = 0; i < nb_rx; i++) {
        struct rte_mbuf *mbuf = bufs[i];
        struct rte_ether_hdr *eth = rte_pktmbuf_mtod(mbuf, struct rte_ether_hdr *);
        uint16_t ether_type = rte_be_to_cpu_16(eth->ether_type);

        if (ether_type == RTE_ETHER_TYPE_ARP) {
            handle_arp(mbuf, port_id);
        } else if (ether_type == RTE_ETHER_TYPE_IPV4) {
            handle_ipv4(mbuf, port_id, pool);
        } else {
            rte_pktmbuf_free(mbuf);
        }
    }
}

/* ---- Setup helpers --------------------------------------------------------- */

static uint32_t ip_from_str(const char *s)
{
    struct in_addr a;
    inet_aton(s, &a);
    return rte_be_to_cpu_32(a.s_addr);
}

static void init_port(uint16_t port_id, struct rte_mempool *pool)
{
    struct rte_eth_conf port_conf = {0};

    if (rte_eth_dev_configure(port_id, 1, 1, &port_conf) < 0)
        rte_exit(EXIT_FAILURE, "Cannot configure port %u\n", port_id);

    if (rte_eth_rx_queue_setup(port_id, 0, RX_RING_SIZE,
            rte_eth_dev_socket_id(port_id), NULL, pool) < 0)
        rte_exit(EXIT_FAILURE, "RX queue setup failed for port %u\n", port_id);

    if (rte_eth_tx_queue_setup(port_id, 0, TX_RING_SIZE,
            rte_eth_dev_socket_id(port_id), NULL) < 0)
        rte_exit(EXIT_FAILURE, "TX queue setup failed for port %u\n", port_id);

    if (rte_eth_dev_start(port_id) < 0)
        rte_exit(EXIT_FAILURE, "Cannot start port %u\n", port_id);

    rte_eth_promiscuous_enable(port_id);
}

int main(int argc, char *argv[])
{
    int ret;
    struct rte_mempool *mbuf_pool;

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    ret = rte_eal_init(argc, argv);
    if (ret < 0)
        rte_exit(EXIT_FAILURE, "Cannot initialize EAL\n");

    uint16_t nb_ports = rte_eth_dev_count_avail();
    if (nb_ports < 2)
        rte_exit(EXIT_FAILURE, "Need at least 2 ports. Found: %u\n", nb_ports);

    mbuf_pool = rte_pktmbuf_pool_create("MBUF_POOL", NUM_MBUFS * nb_ports * 2,
                                         MBUF_CACHE_SIZE, 0,
                                         RTE_MBUF_DEFAULT_BUF_SIZE, rte_socket_id());
    if (mbuf_pool == NULL)
        rte_exit(EXIT_FAILURE, "Cannot create mbuf pool\n");

    init_port(PORT_VM2, mbuf_pool);
    init_port(PORT_VM1, mbuf_pool);

    /* ---- Configure the two "router interfaces" -----------------------
     * EDIT THESE if your subnets/IPs differ.
     */
    ifaces[0].port_id = PORT_VM2;
    ifaces[0].ip      = ip_from_str("192.168.100.1");   /* gateway seen by VM2 */
    ifaces[0].netmask = ip_from_str("255.255.255.0");
    ifaces[0].subnet  = ifaces[0].ip & ifaces[0].netmask;
    ifaces[0].is_default_route = 1;                       /* [ADDED] VM2 is the internet path */
    ifaces[0].next_hop_ip = ip_from_str("192.168.100.2"); /* [ADDED] VM2's real IP -- the actual next hop */
    rte_eth_macaddr_get(PORT_VM2, &ifaces[0].mac);

    ifaces[1].port_id = PORT_VM1;
    ifaces[1].ip      = ip_from_str("192.168.200.1");   /* gateway seen by VM1 */
    ifaces[1].netmask = ip_from_str("255.255.255.0");
    ifaces[1].subnet  = ifaces[1].ip & ifaces[1].netmask;
    ifaces[1].is_default_route = 0;                     /* [ADDED] VM1 side is never the internet path */
    rte_eth_macaddr_get(PORT_VM1, &ifaces[1].mac);

    memset(arp_table, 0, sizeof(arp_table));

    char m0[18], m1[18];
    mac_to_str(&ifaces[0].mac, m0, sizeof(m0));
    mac_to_str(&ifaces[1].mac, m1, sizeof(m1));

    printf("============================================================\n");
    printf("  DPDK LAYER-3 ROUTER (real routed hop, two subnets)\n");
    printf("============================================================\n");
    printf("Port 0 (VM2 side): IP 192.168.100.1  MAC %s  [DEFAULT ROUTE -> 192.168.100.2]\n", m0);
    printf("Port 1 (VM1 side): IP 192.168.200.1  MAC %s\n", m1);
    printf("============================================================\n");
    printf("[INFO] Press Ctrl+C to stop\n\n");

    while (running) {
        process_port(PORT_VM2, mbuf_pool);
        process_port(PORT_VM1, mbuf_pool);
    }

    printf("\n[INFO] Stopping ports...\n");
    rte_eth_dev_stop(PORT_VM2);
    rte_eth_dev_stop(PORT_VM1);
    rte_eth_dev_close(PORT_VM2);
    rte_eth_dev_close(PORT_VM1);

    return 0;
}