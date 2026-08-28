/* [FIXED] Must be defined before ANY header is included. ndpi_api.h uses
 * BSD-style typedefs (u_int, u_char) that glibc only declares when
 * _DEFAULT_SOURCE (or _BSD_SOURCE) is active. Under a strict -std=c11/c99
 * build (typical for DPDK's meson build) glibc hides them by default, and
 * once <stdio.h> or any other header pulls in <features.h> the feature-test
 * macros are locked in for the whole translation unit -- so this has to be
 * the very first thing in the file. */
#define _DEFAULT_SOURCE
#include <sys/types.h>

/*
 * DPDK L3 Router, VM2 <-> VM1, with VM1 as the internet gateway
 *
 * ARCHITECTURE (confirmed addressing):
 *   VM2  (client)   192.168.100.2/24   gw 192.168.100.1
 *   DPDK Port 0                        192.168.100.1   (VM2-facing)
 *   DPDK Port 1                        192.168.200.1   (VM1-facing)
 *   VM1  (server)   192.168.200.2/24   -- also has its OWN separate NIC
 *                                         (enp0s3, VirtualBox NAT, 10.0.2.15)
 *                                         bridged to the real internet.
 *
 * This DPDK box only ever has 2 ports. It does NOT do NAT and does NOT
 * reach the internet directly. Its only job for internet-bound traffic
 * is: "if the destination isn't VM2's subnet or VM1's subnet, hand it to
 * VM1 -- VM1 will NAT it out to the real internet on its own NIC using
 * ordinary Linux iptables MASQUERADE, completely outside this program."
 *
 * Traffic flow for e.g. VM2 curling a real website:
 *   VM2 --(port0)--> DPDK+nDPI --(port1)--> VM1 --(enp0s3, real NAT)--> Internet
 *   VM2 <--(port0)-- DPDK+nDPI <--(port1)-- VM1 <--(enp0s3, real NAT)-- Internet
 * Both legs pass through this router and get inspected by nDPI -- no
 * address rewriting happens on this box, so the 5-tuple nDPI sees never
 * needs to be reconciled across a NAT boundary here. This also means
 * nDPI always sees a clean, unmodified packet in both directions, which
 * is exactly what lets it decode real headers/payload without a NAT
 * boundary getting in the way.
 *
 * VM1 needs (run once per VM1 boot, or persist via /etc/sysctl.conf +
 * iptables-persistent):
 *   sudo sysctl -w net.ipv4.ip_forward=1
 *   sudo iptables -t nat -A POSTROUTING -o enp0s3 -j MASQUERADE
 *   sudo ip route add 192.168.100.0/24 via 192.168.200.1
 * (enp0s3 stays on VirtualBox NAT mode -- do NOT bridge it. VM1 isn't
 * using DPDK/vfio-pci on that NIC, it's ordinary kernel routing, and
 * VirtualBox's NAT mode handles double-NAT'd traffic like this fine.)
 *
 * Expected NIC MACs for this DPDK VM's two ports (confirm from the
 * startup banner, which reads these live off the NIC via
 * rte_eth_macaddr_get() -- never hardcode a MAC into the forwarding
 * logic, only use this to sanity check DPDK bound the port you expect):
 *   Port 0 (VM2 side): 08:00:27:41:AA:B7
 *   Port 1 (VM1 side): 08:00:27:BC:EE:4C
 *
 * IMPORTANT: DPDK assigns port numbers by ascending PCI address, not by
 * the order of -a flags on the command line. Verify the real mapping the
 * first time you run this by watching the startup banner and the [ARP]
 * Learned lines below it. Confirm from the live log every time you
 * change which physical/virtual NIC something is wired into.
 *
 * Launch (only 2 -a flags needed -- no third WAN port on this box):
 *   sudo ./jetson_router -l 0-3 -n 4 \
 *       -a <PCI of VM2-facing NIC> -a <PCI of VM1-facing NIC>
 *
 * [FIXED] This version has been adjusted to match the nDPI API actually
 * installed on this system, which differs from the newer "wrapped"
 * ndpi_protocol API in a few ways:
 *   - struct ndpi_proto (aka ndpi_protocol) exposes master_protocol /
 *     app_protocol directly -- there is no nested ".proto" member.
 *   - the detected HTTP/TLS hostname lives on the flow itself as
 *     host_server_name (a fixed char[80] array), not under flow->http.host.
 *     On this nDPI build the same field carries HTTP Host, TLS SNI, and
 *     DNS query names -- one field covers "what domain was this about."
 *   - ndpi_init_detection_module() takes an ndpi_init_prefs value
 *     (use ndpi_no_prefs), not a NULL void pointer.
 *   - ndpi_finalize_initialization() returns void in this version, so its
 *     return value must not be compared against anything.
 *   - ndpi_detection_process_packet() takes 5 parameters in this version,
 *     no trailing src/dst/input_info argument.
 *
 * [MERGED] This file combines the VM1-as-gateway routing design (2 DPDK
 * ports, no NAT inside DPDK -- VM1's own separate NIC does real NAT via
 * iptables) with the detailed per-flow packet dump (full IPv4/TCP/UDP
 * header fields, nDPI protocol category, and a hex+ASCII payload
 * preview) that was previously only in the 3-port NAT experiment. The
 * third WAN interface and all in-DPDK NAT logic from that experiment
 * have been intentionally dropped -- not needed with this architecture.
 */

#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <signal.h>
#include <string.h>
#include <arpa/inet.h>
#include <stdlib.h>

#include <rte_eal.h>
#include <rte_ethdev.h>
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_arp.h>
#include <rte_ip.h>
#include <rte_icmp.h>
#include <rte_byteorder.h>
#include <rte_cycles.h>
#include <rte_pdump.h>

#include <ndpi_api.h>

#define RX_RING_SIZE 128
#define TX_RING_SIZE 128
#define NUM_MBUFS    8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE   32
#define ARP_TABLE_SIZE 32

#define NDPI_MAX_FLOWS 4096

#define PORT_VM2 0   /* physical/virtual port 0, confirm from live log */
#define PORT_VM1 1   /* physical/virtual port 1, confirm from live log */

/* [ADDED] Per-packet printf() is a syscall -- doing one per packet caps
 * this router at a few thousand pps regardless of how fast DPDK itself
 * could go. Verbose per-packet lines are OFF by default; use the always-on
 * [STATS] summary (see print_stats()) for throughput testing instead.
 * Flip g_verbose to 1 only when debugging connectivity at low/no load. */
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

/* [ADDED] VM1's internal IP doubles as this router's "default gateway"
 * for anything that isn't VM2's or VM1's own subnet -- i.e. real
 * internet destinations. Set after ifaces[1] is populated in main().
 * Traffic routed here goes out port 1 addressed to VM1's MAC (not the
 * real destination's MAC, since the real destination isn't on-link) --
 * exactly like a host sending to its default gateway. */
static uint32_t default_gw_ip;

/* Forward declarations -- these are defined further down in the file but
 * used earlier (e.g. inside dpi_process_ipv4()), so the compiler needs to
 * see their prototypes first or it will silently assume an implicit
 * int-returning declaration and then error out when it hits the real,
 * differently-typed static definition later. */
static uint32_t ip_from_str(const char *s);
static void ip_to_str(uint32_t ip, char *buf, size_t len);
static void mac_to_str(const struct rte_ether_addr *m, char *buf, size_t len);

/*
 * [ADDED] nDPI-based Deep Packet Inspection.
 *
 * nDPI receives the IPv4 packet starting at the IP header (Layer 3), keeps
 * state per 5-tuple, and classifies the traffic. For HTTP/1.1, nDPI can also
 * expose the Host header (and, on this build, TLS SNI / DNS query name)
 * after enough packets of the flow have been seen.
 *
 * This is intentionally kept separate from the existing routing logic:
 * packets are inspected, but routing/ARP/ICMP behavior is otherwise unchanged.
 */
struct dpi_flow_key {
    uint32_t ip_a;
    uint32_t ip_b;
    uint16_t port_a;
    uint16_t port_b;
    uint8_t l4_proto;
};

struct dpi_flow_entry {
    int valid;
    struct dpi_flow_key key;
    struct ndpi_flow_struct *flow;
    uint64_t last_seen_ms;
    uint16_t last_proto;
    char last_host[80];
};

static struct ndpi_detection_module_struct *ndpi_mod;
static struct dpi_flow_entry dpi_flows[NDPI_MAX_FLOWS];

static uint64_t dpi_now_ms(void)
{
    return (rte_get_timer_cycles() * 1000ULL) / rte_get_timer_hz();
}

static uint32_t dpi_hash_key(const struct dpi_flow_key *k)
{
    uint32_t h = 2166136261u;

    h ^= k->ip_a;     h *= 16777619u;
    h ^= k->ip_b;     h *= 16777619u;
    h ^= k->port_a;   h *= 16777619u;
    h ^= k->port_b;   h *= 16777619u;
    h ^= k->l4_proto; h *= 16777619u;

    return h;
}

static void dpi_make_key(uint32_t src_ip, uint16_t src_port,
                         uint32_t dst_ip, uint16_t dst_port,
                         uint8_t l4_proto, struct dpi_flow_key *key)
{
    /*
     * Canonicalize the two endpoints so packets in both directions map to
     * the same nDPI flow entry.
     */
    if (src_ip < dst_ip ||
        (src_ip == dst_ip && src_port <= dst_port)) {
        key->ip_a = src_ip;
        key->port_a = src_port;
        key->ip_b = dst_ip;
        key->port_b = dst_port;
    } else {
        key->ip_a = dst_ip;
        key->port_a = dst_port;
        key->ip_b = src_ip;
        key->port_b = src_port;
    }

    key->l4_proto = l4_proto;
}

static int dpi_key_equal(const struct dpi_flow_key *a,
                         const struct dpi_flow_key *b)
{
    return a->ip_a == b->ip_a &&
           a->ip_b == b->ip_b &&
           a->port_a == b->port_a &&
           a->port_b == b->port_b &&
           a->l4_proto == b->l4_proto;
}

static void dpi_free_entry(struct dpi_flow_entry *entry)
{
    if (!entry->valid)
        return;

    if (entry->flow != NULL)
        ndpi_free_flow(entry->flow);

    memset(entry, 0, sizeof(*entry));
}

static struct dpi_flow_entry *dpi_get_flow(const struct dpi_flow_key *key,
                                           uint64_t now_ms)
{
    uint32_t start = dpi_hash_key(key) % NDPI_MAX_FLOWS;
    struct dpi_flow_entry *oldest = NULL;

    for (uint32_t n = 0; n < NDPI_MAX_FLOWS; n++) {
        uint32_t pos = (start + n) % NDPI_MAX_FLOWS;
        struct dpi_flow_entry *entry = &dpi_flows[pos];

        if (!entry->valid) {
            entry->flow = ndpi_calloc(1,
                                      ndpi_detection_get_sizeof_ndpi_flow_struct());
            if (entry->flow == NULL)
                return NULL;

            entry->valid = 1;
            entry->key = *key;
            entry->last_seen_ms = now_ms;
            entry->last_proto = UINT16_MAX; /* sentinel: "never printed yet" */
            entry->last_host[0] = '\0';
            return entry;
        }

        if (dpi_key_equal(&entry->key, key)) {
            entry->last_seen_ms = now_ms;
            return entry;
        }

        if (oldest == NULL ||
            entry->last_seen_ms < oldest->last_seen_ms)
            oldest = entry;
    }

    /*
     * Table full. Reuse the oldest flow. This is only a bounded flow cache;
     * the forwarding path itself is unchanged.
     */
    if (oldest != NULL) {
        dpi_free_entry(oldest);

        oldest->flow = ndpi_calloc(1,
                                    ndpi_detection_get_sizeof_ndpi_flow_struct());
        if (oldest->flow == NULL)
            return NULL;

        oldest->valid = 1;
        oldest->key = *key;
        oldest->last_seen_ms = now_ms;
        return oldest;
    }

    return NULL;
}

/* [ADDED] Full packet detail dump -- IP header fields, TCP/UDP header
 * fields, nDPI's protocol + category, and a bounded hex+ASCII preview of
 * the actual payload bytes. Called once per flow (whenever the DPI
 * summary line already fires), not per packet, so it stays readable and
 * doesn't tank throughput the way per-packet printf would. */
#define DPI_PAYLOAD_DUMP_MAX 64

static void dpi_hex_dump(const uint8_t *data, uint16_t len)
{
    uint16_t shown = (len < DPI_PAYLOAD_DUMP_MAX) ? len : DPI_PAYLOAD_DUMP_MAX;

    for (uint16_t off = 0; off < shown; off += 16) {
        printf("    %04u  ", off);

        for (uint16_t i = 0; i < 16; i++) {
            if (off + i < shown)
                printf("%02X ", data[off + i]);
            else
                printf("   ");
        }

        printf(" ");
        for (uint16_t i = 0; i < 16 && off + i < shown; i++) {
            uint8_t b = data[off + i];
            printf("%c", (b >= 32 && b < 127) ? b : '.');
        }
        printf("\n");
    }

    if (len > DPI_PAYLOAD_DUMP_MAX)
        printf("    ... (%u more bytes not shown)\n", len - DPI_PAYLOAD_DUMP_MAX);
}

static void dpi_print_packet_detail(struct rte_ipv4_hdr *ip,
                                     uint8_t l4_proto,
                                     const void *l4_hdr,
                                     ndpi_protocol detected,
                                     const char *proto_name,
                                     const char *sstr, const char *dstr,
                                     uint16_t src_port, uint16_t dst_port)
{
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    uint16_t total_len = rte_be_to_cpu_16(ip->total_length);

    ndpi_protocol_category_t category = ndpi_get_proto_category(ndpi_mod, detected);
    const char *category_name = ndpi_category_get_name(ndpi_mod, category);

    printf("  -- IPv4 header -----------------------------------------\n");
    printf("    version=%u  ihl=%u bytes  tos=0x%02X  total_len=%u\n",
           (ip->version_ihl >> 4) & 0x0F, ihl, ip->type_of_service, total_len);
    printf("    id=0x%04X  ttl=%u  proto=%u  hdr_checksum=0x%04X\n",
           rte_be_to_cpu_16(ip->packet_id), ip->time_to_live,
           ip->next_proto_id, rte_be_to_cpu_16(ip->hdr_checksum));
    printf("    src=%s  dst=%s\n", sstr, dstr);

    if (l4_proto == IPPROTO_TCP) {
        const struct rte_tcp_hdr *tcp = l4_hdr;
        printf("  -- TCP header -------------------------------------------\n");
        printf("    src_port=%u  dst_port=%u\n", src_port, dst_port);
        printf("    seq=%u  ack=%u  window=%u  checksum=0x%04X\n",
               rte_be_to_cpu_32(tcp->sent_seq), rte_be_to_cpu_32(tcp->recv_ack),
               rte_be_to_cpu_16(tcp->rx_win), rte_be_to_cpu_16(tcp->cksum));
        printf("    flags: %s%s%s%s%s%s\n",
               (tcp->tcp_flags & RTE_TCP_SYN_FLAG) ? "SYN " : "",
               (tcp->tcp_flags & RTE_TCP_ACK_FLAG) ? "ACK " : "",
               (tcp->tcp_flags & RTE_TCP_FIN_FLAG) ? "FIN " : "",
               (tcp->tcp_flags & RTE_TCP_RST_FLAG) ? "RST " : "",
               (tcp->tcp_flags & RTE_TCP_PSH_FLAG) ? "PSH " : "",
               (tcp->tcp_flags & RTE_TCP_URG_FLAG) ? "URG " : "");
    } else if (l4_proto == IPPROTO_UDP) {
        const struct rte_udp_hdr *udp = l4_hdr;
        printf("  -- UDP header -------------------------------------------\n");
        printf("    src_port=%u  dst_port=%u  length=%u  checksum=0x%04X\n",
               src_port, dst_port, rte_be_to_cpu_16(udp->dgram_len),
               rte_be_to_cpu_16(udp->dgram_cksum));
    }

    printf("  -- nDPI classification -----------------------------------\n");
    printf("    protocol=%s  category=%s (id=%d)\n",
           proto_name, category_name ? category_name : "Unknown", category);

    uint8_t l4_hdr_len = (l4_proto == IPPROTO_TCP) ?
        (((const struct rte_tcp_hdr *)l4_hdr)->data_off >> 4) * 4 :
        sizeof(struct rte_udp_hdr);
    const uint8_t *payload = (const uint8_t *)l4_hdr + l4_hdr_len;
    uint16_t payload_len = total_len - ihl - l4_hdr_len;

    if (payload_len > 0 && payload_len < total_len) {
        printf("  -- Payload (%u bytes, showing up to %d) ------------------\n",
               payload_len, DPI_PAYLOAD_DUMP_MAX);
        dpi_hex_dump(payload, payload_len);
    } else {
        printf("  -- Payload: none (header-only packet, e.g. bare SYN/ACK) --\n");
    }
    printf("  -----------------------------------------------------------\n\n");
}

static void dpi_process_ipv4(struct rte_mbuf *mbuf,
                              struct rte_ipv4_hdr *ip)
{
    uint8_t l4_proto = ip->next_proto_id;

    if (l4_proto != IPPROTO_TCP && l4_proto != IPPROTO_UDP)
        return;

    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    uint16_t total_len = rte_be_to_cpu_16(ip->total_length);

    if (ihl < sizeof(struct rte_ipv4_hdr) ||
        total_len < ihl + 4 ||
        total_len > rte_pktmbuf_data_len(mbuf))
        return;

    uint16_t src_port = 0;
    uint16_t dst_port = 0;

    if (l4_proto == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcp =
            (struct rte_tcp_hdr *)((uint8_t *)ip + ihl);
        src_port = rte_be_to_cpu_16(tcp->src_port);
        dst_port = rte_be_to_cpu_16(tcp->dst_port);
    } else {
        struct rte_udp_hdr *udp =
            (struct rte_udp_hdr *)((uint8_t *)ip + ihl);
        src_port = rte_be_to_cpu_16(udp->src_port);
        dst_port = rte_be_to_cpu_16(udp->dst_port);
    }

    uint32_t src_ip = rte_be_to_cpu_32(ip->src_addr);
    uint32_t dst_ip = rte_be_to_cpu_32(ip->dst_addr);

    struct dpi_flow_key key;
    dpi_make_key(src_ip, src_port, dst_ip, dst_port, l4_proto, &key);

    uint64_t now_ms = dpi_now_ms();
    struct dpi_flow_entry *entry = dpi_get_flow(&key, now_ms);

    if (entry == NULL)
        return;

    /*
     * nDPI expects an IPv4 packet beginning at the Layer-3 header.
     * It performs protocol classification using the packet plus the
     * per-flow state we keep above.
     */
    /* [FIXED] This nDPI version's ndpi_detection_process_packet() takes
     * only 5 parameters -- no trailing src/dst/input_info argument. */
    ndpi_protocol detected =
        ndpi_detection_process_packet(ndpi_mod, entry->flow,
                                      (const unsigned char *)ip,
                                      total_len, now_ms);

    /* [FIXED] This nDPI version exposes master_protocol/app_protocol
     * directly on ndpi_protocol -- there is no nested ".proto" member.
     * [CHANGED] No longer returning early when both are 0 (Unknown) --
     * we still print once per flow so it's visible that DPI ran and made
     * a determination, even when that determination is "can't tell"
     * (e.g. plain iperf3 traffic has no application-layer signature,
     * and the very first SYN of any TCP flow is always Unknown until
     * enough of the handshake/data has been seen). */
    char proto_name[128];
    ndpi_protocol2name(ndpi_mod, detected, proto_name, sizeof(proto_name));

    /* [FIXED] The detected hostname lives on the flow as
     * host_server_name, a fixed char[80] array -- not flow->http.host.
     * On this nDPI build this same field is populated for HTTP (Host
     * header), TLS (SNI from the ClientHello), and DNS query names. */
    const char *host = "";
    if (entry->flow->host_server_name[0] != '\0')
        host = (const char *)entry->flow->host_server_name;

    uint16_t proto_id = detected.master_protocol;
    if (detected.app_protocol != 0)
        proto_id = detected.app_protocol;

    if (proto_id != entry->last_proto ||
        strcmp(host, entry->last_host) != 0) {

        char sstr[16], dstr[16];
        ip_to_str(src_ip, sstr, sizeof(sstr));
        ip_to_str(dst_ip, dstr, sizeof(dstr));

        /* Domain name (when nDPI has one -- HTTP Host, TLS SNI, or DNS
         * query name) leads the line; protocol name and 5-tuple follow. */
        if (host[0] != '\0') {
            printf("[DPI] Domain=%s | %s | %s:%u -> %s:%u\n",
                   host, proto_name, sstr, src_port, dstr, dst_port);

            snprintf(entry->last_host, sizeof(entry->last_host),
                     "%s", host);
        } else {
            printf("[DPI] %s | %s:%u -> %s:%u\n",
                   proto_name, sstr, src_port, dstr, dst_port);
            entry->last_host[0] = '\0';
        }

        /* [MERGED] Full header + payload detail, printed alongside the
         * summary line above -- same once-per-flow-change trigger, so
         * this doesn't add per-packet overhead. */
        void *l4_hdr_ptr = (uint8_t *)ip + ihl;
        dpi_print_packet_detail(ip, l4_proto, l4_hdr_ptr, detected,
                                 proto_name, sstr, dstr, src_port, dst_port);

        entry->last_proto = proto_id;
    }
}

static int dpi_init(void)
{
    /* [FIXED] This version's ndpi_init_detection_module() takes an
     * ndpi_init_prefs value, not a NULL void pointer. */
    ndpi_mod = ndpi_init_detection_module(ndpi_no_prefs);
    if (ndpi_mod == NULL) {
        printf("[WARN] nDPI initialization failed -- DPI disabled\n");
        return -1;
    }

    /* [FIXED -- ROOT CAUSE OF "DPI NEVER PRINTS ANYTHING"]
     * nDPI's protocol detectors are opt-in. Without explicitly enabling a
     * bitmask of protocols to look for, ndpi_detection_process_packet()
     * still runs but classifies every single packet as Unknown (protocol
     * 0), no matter what the traffic actually is -- HTTP, TLS, anything.
     * This must happen AFTER ndpi_init_detection_module() and BEFORE
     * ndpi_finalize_initialization(). */
    NDPI_PROTOCOL_BITMASK protos;
    NDPI_BITMASK_SET_ALL(protos);
    ndpi_set_protocol_detection_bitmask2(ndpi_mod, &protos);

    /* [FIXED] ndpi_finalize_initialization() returns void in this version,
     * so its result must not be compared against anything. */
    ndpi_finalize_initialization(ndpi_mod);

    printf("[INFO] nDPI initialized, version %s\n", ndpi_revision());
    printf("[INFO] nDPI protocol detection bitmask enabled (all protocols)\n");
    return 0;
}

static void dpi_uninit(void)
{
    if (ndpi_mod == NULL)
        return;

    for (int i = 0; i < NDPI_MAX_FLOWS; i++)
        dpi_free_entry(&dpi_flows[i]);

    ndpi_exit_detection_module(ndpi_mod);
    ndpi_mod = NULL;
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

/* [ADDED] Live throughput stats, printed roughly once a second, driven by
 * rte_eth_stats_get() -- counters the NIC driver maintains itself in
 * hardware/software regardless of what our own code does. This means:
 *   - it costs nothing extra per-packet (no manual counter increments
 *     needed anywhere in the hot path)
 *   - "imissed" specifically tells you packets the NIC dropped because
 *     software couldn't keep up (rx_burst not called often enough / not
 *     fast enough) -- this is your #1 signal for "DPDK itself is the
 *     bottleneck" versus "the bottleneck is upstream/downstream of it"
 * Compare this against what iperf3 reports on VM2 (sender) and on
 * VM1 (receiver) to localize exactly where loss happens:
 *   VM2 iperf3 send rate  vs  Port0 RX here   ->  loss on the VM2<->DPDK link
 *   Port0 RX here  vs  Port1 TX here          ->  loss inside the router itself
 *   Port1 TX here  vs  VM1 iperf3 recv rate   ->  loss on the DPDK<->VM1 link
 */
static void print_stats(void)
{
    static uint64_t last_cycles;
    static struct rte_eth_stats prev[2];
    static int first_call = 1;

    uint64_t now = rte_get_timer_cycles();
    uint64_t hz  = rte_get_timer_hz();

    if (first_call) {
        first_call = 0;
        last_cycles = now;
        rte_eth_stats_get(PORT_VM2, &prev[0]);
        rte_eth_stats_get(PORT_VM1, &prev[1]);
        return;
    }

    double elapsed = (double)(now - last_cycles) / (double)hz;
    if (elapsed < 1.0)
        return;

    struct rte_eth_stats cur[2];
    rte_eth_stats_get(PORT_VM2, &cur[0]);
    rte_eth_stats_get(PORT_VM1, &cur[1]);

    static const char *label[2] = { "VM2", "VM1" };

    printf("\n[STATS] ---- %.2fs window ----\n", elapsed);
    for (int i = 0; i < 2; i++) {
        uint64_t rx_pkts  = cur[i].ipackets - prev[i].ipackets;
        uint64_t rx_bytes = cur[i].ibytes   - prev[i].ibytes;
        uint64_t tx_pkts  = cur[i].opackets - prev[i].opackets;
        uint64_t tx_bytes = cur[i].obytes   - prev[i].obytes;
        uint64_t rx_miss  = cur[i].imissed  - prev[i].imissed;   /* dropped by NIC, ring full */
        uint64_t rx_err   = cur[i].ierrors  - prev[i].ierrors;
        uint64_t tx_err   = cur[i].oerrors  - prev[i].oerrors;

        double rx_mbps = (rx_bytes * 8.0) / elapsed / 1e6;
        double tx_mbps = (tx_bytes * 8.0) / elapsed / 1e6;

        printf("  Port %d (%s): RX %8" PRIu64 " pkt/s  %9.2f Mbps | "
               "TX %8" PRIu64 " pkt/s  %9.2f Mbps | rx_miss %" PRIu64
               " rx_err %" PRIu64 " tx_err %" PRIu64 "\n",
               i, label[i],
               (uint64_t)(rx_pkts / elapsed), rx_mbps,
               (uint64_t)(tx_pkts / elapsed), tx_mbps,
               rx_miss, rx_err, tx_err);

        if (rx_miss > 0)
            printf("    ^ Port %d dropped %" PRIu64
                   " incoming packets -- the RX ring filled up because this\n"
                   "      app's poll loop can't drain it fast enough. This IS your bottleneck.\n",
                   i, rx_miss);
    }

    prev[0] = cur[0];
    prev[1] = cur[1];
    last_cycles = now;
}

static void arp_learn(uint16_t port_id, uint32_t ip, const struct rte_ether_addr *mac)
{
    if (ip == 0)
        return;
    int idx = idx_of(port_id);
    struct arp_entry *tbl = arp_table[idx];

    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (tbl[i].valid && tbl[i].ip == ip) {
            tbl[i].mac = *mac;
            return;
        }
    }
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!tbl[i].valid) {
            tbl[i].ip = ip;
            tbl[i].mac = *mac;
            tbl[i].valid = 1;
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
        if (tbl[i].valid && tbl[i].ip == ip) {
            *out = tbl[i].mac;
            return 1;
        }
    }
    return 0;
}

static void send_arp_request(int idx, uint32_t target_ip)
{
    struct rte_mbuf *req = rte_pktmbuf_alloc(mbuf_pool);
    if (req == NULL)
        return;

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
        LOGV("[ARP] Replied on port %u: I am %u.%u.%u.%u\n", in_port,
               (ifaces[idx].ip >> 24) & 0xFF, (ifaces[idx].ip >> 16) & 0xFF,
               (ifaces[idx].ip >> 8) & 0xFF, ifaces[idx].ip & 0xFF);
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
    ip->time_to_live = 64;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    icmp->icmp_type = 0;   /* echo reply */
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

    /* [ADDED] Inspect TCP/UDP traffic with nDPI before routing. This
     * always sees the real, un-rewritten 5-tuple in both directions --
     * this router doesn't NAT anything itself, VM1 does that on its own
     * separate NIC, fully outside this program's view. */
    if (ndpi_mod != NULL)
        dpi_process_ipv4(mbuf, ip);

    /* Addressed to one of our own two IPs? Answer directly, don't forward. */
    for (int i = 0; i < 2; i++) {
        if (dst_ip == ifaces[i].ip) {
            uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
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

    /* Subnet broadcast? Drop cleanly, never ARP for it. */
    for (int i = 0; i < 2; i++) {
        uint32_t bcast = ifaces[i].subnet | ~ifaces[i].netmask;
        if (dst_ip == bcast) {
            rte_pktmbuf_free(mbuf);
            return;
        }
    }

    /* Route: which of our two known subnets does this belong to? */
    int out_idx = -1;
    for (int i = 0; i < 2; i++) {
        if ((dst_ip & ifaces[i].netmask) == ifaces[i].subnet) {
            out_idx = i;
            break;
        }
    }

    /* [ADDED] Destination is neither VM2's nor VM1's subnet -- treat VM1
     * as the default gateway for everything else (real internet
     * destinations). The packet goes out port 1, but the next-hop MAC we
     * ARP for is VM1's own IP (default_gw_ip), NOT the packet's real
     * destination -- the real destination isn't on-link, so this is
     * exactly what any host does when sending to its default gateway.
     * VM1 will NAT it out to the real internet on its own separate NIC. */
    int next_hop_is_gateway = 0;
    uint32_t next_hop_ip = dst_ip;

    if (out_idx < 0) {
        out_idx = idx_of(PORT_VM1);
        next_hop_ip = default_gw_ip;
        next_hop_is_gateway = 1;
    }

    uint16_t out_port = ifaces[out_idx].port_id;
    if (out_port == in_port) {
        /* Only meaningful for the on-link case; the gateway case can
         * never trigger this since VM2 always arrives on port 0 and the
         * gateway path always goes out port 1. Kept for the on-link path. */
        rte_pktmbuf_free(mbuf);
        return;
    }

    if (ip->time_to_live <= 1) {
        rte_pktmbuf_free(mbuf);
        return;
    }
    ip->time_to_live--;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

    struct rte_ether_addr next_hop_mac;
    if (!arp_lookup(out_idx, next_hop_ip, &next_hop_mac)) {
        send_arp_request(out_idx, next_hop_ip);
        rte_pktmbuf_free(mbuf);
        return;
    }

    rte_ether_addr_copy(&next_hop_mac, &eth->dst_addr);
    rte_ether_addr_copy(&ifaces[out_idx].mac, &eth->src_addr);

    char sstr[16], dstr[16];
    ip_to_str(src_ip, sstr, sizeof(sstr));
    ip_to_str(dst_ip, dstr, sizeof(dstr));
    const char *proto = ip->next_proto_id == IPPROTO_ICMP ? "ICMP" :
                         ip->next_proto_id == IPPROTO_UDP  ? "UDP"  :
                         ip->next_proto_id == IPPROTO_TCP  ? "TCP"  : "OTHER";

    if (rte_eth_tx_burst(out_port, 0, &mbuf, 1) == 0) {
        rte_pktmbuf_free(mbuf);
    } else if (next_hop_is_gateway) {
        LOGV("[ROUTE-GW] %s: %s -> %s | in=port%u out=port%u (via VM1 gateway)\n",
               proto, sstr, dstr, in_port, out_port);
    } else {
        LOGV("[ROUTE] %s: %s -> %s | in=port%u out=port%u\n",
               proto, sstr, dstr, in_port, out_port);
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

    /* [ADDED] rte_pdump_init() starts the multi-process IPC channel that
     * the dpdk-pdump helper tool (and Wireshark, via a FIFO) connects to
     * in order to capture packets straight off these DPDK-bound ports.
     * Without this call, dpdk-pdump has nothing to attach to -- these
     * ports have zero kernel presence, so plain tcpdump/Wireshark can
     * never see them directly. This call is cheap and safe to leave in
     * even when you're not actively capturing. */
    ret = rte_pdump_init();
    if (ret < 0)
        printf("[WARN] rte_pdump_init() failed (%d) -- packet capture via dpdk-pdump won't work\n", ret);

    /* [ADDED] Initialize nDPI. Router continues even if nDPI is unavailable. */
    dpi_init();

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

    /* [ADDED] VM1's own internal IP (192.168.200.2) is this router's
     * default gateway for anything outside the two known subnets. VM1
     * must actually be configured to accept and NAT this traffic --
     * see the header comment for the exact sysctl/iptables/ip route
     * commands to run on VM1. */
    default_gw_ip = ip_from_str("192.168.200.2");

    char m0[18], m1[18];
    mac_to_str(&ifaces[0].mac, m0, sizeof(m0));
    mac_to_str(&ifaces[1].mac, m1, sizeof(m1));

    char gw_str[16];
    ip_to_str(default_gw_ip, gw_str, sizeof(gw_str));

    printf("============================================================\n");
    printf("  DPDK ROUTER: VM2 <-> VM1 (VM1 = internet gateway)\n");
    printf("============================================================\n");
    printf("Port 0 (VM2 side): IP 192.168.100.1  MAC %s\n", m0);
    printf("Port 1 (VM1 side): IP 192.168.200.1  MAC %s\n", m1);
    printf("Default gateway for non-local traffic: %s (VM1)\n", gw_str);
    printf("============================================================\n");
    printf("[INFO] Expected MACs -- Port 0: 08:00:27:41:AA:B7, Port 1: 08:00:27:BC:EE:4C\n");
    printf("[INFO] If the MACs printed above don't match, DPDK bound the NICs\n");
    printf("[INFO] to the ports in the opposite order from what you expected --\n");
    printf("[INFO] fix by swapping the order of the -a PCI arguments at launch.\n");
    printf("[INFO] Traffic to anything outside 192.168.100.0/24 and\n");
    printf("[INFO] 192.168.200.0/24 is sent to VM1 (%s) as the gateway.\n", gw_str);
    printf("[INFO] VM1 must have ip_forward=1, an iptables MASQUERADE rule on\n");
    printf("[INFO] its internet-facing NIC, and a route back for 192.168.100.0/24\n");
    printf("[INFO] via this router -- see the header comment for exact commands.\n");
    printf("[INFO] Watch the ARP Learned lines below to confirm end-to-end reachability.\n");
    printf("[INFO] Live throughput stats print automatically every ~1s below.\n");
    printf("[INFO] For packet-level capture in Wireshark, in another terminal run e.g.:\n");
    printf("[INFO]   sudo dpdk-pdump -- --pdump 'port=%%u,queue=*,rx-dev=/tmp/port0.pcap'\n");
    printf("[INFO]   (see full instructions in the accompanying writeup)\n");
    printf("[INFO] Press Ctrl+C to stop\n\n");

    while (running) {
        process_port(PORT_VM2);
        process_port(PORT_VM1);
        print_stats();
    }

    dpi_uninit();
    rte_pdump_uninit();
    printf("\n[INFO] Stopping ports...\n");
    rte_eth_dev_stop(PORT_VM2);
    rte_eth_dev_stop(PORT_VM1);
    rte_eth_dev_close(PORT_VM2);
    rte_eth_dev_close(PORT_VM1);

    return 0;
}
