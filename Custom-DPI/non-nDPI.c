/*
 * DPDK L3 Router, VM2 <-> VM1, with VM1 as the internet gateway
 * -- NO nDPI, NO external DPI library of any kind --
 *
 * This version replaces nDPI entirely. There is no protocol signature
 * database anywhere in this file. Every single flow starts out
 * classified as UNKNOWN by this program -- not "unknown to nDPI", but
 * genuinely unknown to THIS code, because this code has no built-in
 * knowledge of any protocol at all until you tell it what to look for.
 *
 * Classification here works by one mechanism only: hand-written
 * structural/grammar checks against the RFC-documented wire format of a
 * specific protocol, run directly against the raw payload bytes DPDK
 * already handed you. If a protocol's format is never coded as a
 * matcher below, this program will call it UNKNOWN forever, no matter
 * how common that protocol actually is on the wire -- there is no
 * fallback database to consult. This is the entire point: proving
 * classification here comes only from grammar you wrote, not from a
 * library's prior knowledge.
 *
 * Four example matchers are included, each checking a real documented
 * wire format:
 *   - HTTP/1.x request line   (RFC 7230 request-line grammar)
 *   - HTTP/1.x response line  (RFC 7230 status-line grammar)
 *   - DNS message header      (RFC 1035 fixed 12-byte header structure)
 *   - TLS ClientHello         (RFC 8446 record + handshake header,
 *                               including SNI extension parsing)
 * Add more by writing a function with the same signature and appending
 * it to `matchers[]` -- see CUSTOM_PROTOCOL_MATCHERS below.
 *
 * ARCHITECTURE (unchanged from the routing/gateway design):
 *   VM2  (client)   192.168.100.2/24   gw 192.168.100.1
 *   DPDK Port 0                        192.168.100.1   (VM2-facing)
 *   DPDK Port 1                        192.168.200.1   (VM1-facing)
 *   VM1  (server)   192.168.200.2/24   -- also has its OWN separate NIC
 *                                         (enp0s3, VirtualBox NAT, 10.0.2.15)
 *                                         bridged to the real internet.
 *
 * This DPDK box only ever has 2 ports, does NOT NAT, and does NOT reach
 * the internet directly -- VM1's own NIC does real NAT via iptables.
 * See the previous nDPI-based file's header comment for the exact VM1
 * sysctl/iptables/ip route commands; they are unchanged by this rewrite.
 *
 * Expected NIC MACs for this DPDK VM's two ports (confirm from the
 * startup banner):
 *   Port 0 (VM2 side): 08:00:27:41:AA:B7
 *   Port 1 (VM1 side): 08:00:27:BC:EE:4C
 *
 * Launch (same as before, 2 -a flags):
 *   sudo ./jetson_router_noNdpi -l 0-3 -n 4 \
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
#include <math.h>

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

#define RX_RING_SIZE 128
#define TX_RING_SIZE 128
#define NUM_MBUFS    8191
#define MBUF_CACHE_SIZE 250
#define BURST_SIZE   32
#define ARP_TABLE_SIZE 32

#define FLOW_TABLE_SIZE 4096

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
 * CUSTOM PROTOCOL MATCHERS -- the entire "intelligence" of this
 * program's classification lives here, and nowhere else. No external
 * database is consulted anywhere. Each matcher checks the payload
 * against ONE documented wire format and fills a short human-readable
 * detail string on success.
 * ============================================================ */
#define MATCH_DETAIL_MAX 256

typedef int (*proto_matcher_fn)(const uint8_t *payload, uint16_t len,
                                 char *detail, size_t detail_sz);

/* ---- HTTP/1.x request line: METHOD SP Request-URI SP HTTP-Version CRLF
 * (RFC 7230 section 3.1.1) ---- */
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

/* ---- HTTP/1.x status line: HTTP-Version SP Status-Code SP Reason-Phrase
 * CRLF (RFC 7230 section 3.1.2) ---- */
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

/* ---- DNS message header, RFC 1035 section 4.1.1: fixed 12-byte header
 * (ID, flags, QDCOUNT, ANCOUNT, NSCOUNT, ARCOUNT), followed by the
 * question section if QDCOUNT > 0. We validate the header's structural
 * plausibility (not just guess from port 53) and, if it looks like a
 * query, parse the QNAME out of the wire format ourselves. ---- */
static int match_dns(const uint8_t *payload, uint16_t len,
                      char *detail, size_t detail_sz)
{
    if (payload == NULL || len < 12)
        return 0;

    uint16_t flags   = (payload[2] << 8) | payload[3];
    uint16_t qdcount = (payload[4] << 8) | payload[5];
    uint16_t ancount = (payload[6] << 8) | payload[7];
    uint16_t nscount = (payload[8] << 8) | payload[9];
    uint16_t arcount = (payload[10] << 8) | payload[11];

    uint8_t opcode = (flags >> 11) & 0x0F;
    uint8_t qr     = (flags >> 15) & 0x01;

    /* Structural plausibility, not signature matching: standard opcodes
     * only (0=query,1=iquery,2=status,4=notify,5=update), and sane
     * record counts -- arbitrary binary data essentially never satisfies
     * all of these simultaneously by chance. */
    if (opcode > 5)
        return 0;
    if (qdcount > 16 || ancount > 64 || nscount > 64 || arcount > 64)
        return 0;
    if (qdcount == 0)
        return 0; /* require at least one question to parse a name from */

    /* Parse QNAME: sequence of length-prefixed labels terminated by a
     * zero-length label. No compression pointer handling needed for the
     * very first question in a query (nothing to point backward to
     * yet). */
    const uint8_t *p = payload + 12;
    const uint8_t *end = payload + len;
    char qname[256];
    size_t qname_off = 0;

    while (p < end && *p != 0) {
        uint8_t label_len = *p;
        if (label_len > 63)
            return 0; /* invalid per RFC 1035 label length limit */
        p++;
        if (p + label_len > end)
            return 0;
        if (qname_off + label_len + 1 >= sizeof(qname))
            return 0;
        if (qname_off > 0)
            qname[qname_off++] = '.';
        memcpy(qname + qname_off, p, label_len);
        qname_off += label_len;
        p += label_len;
    }
    if (p >= end)
        return 0; /* ran off the end without a terminating zero label */
    qname[qname_off] = '\0';

    snprintf(detail, detail_sz, "%s query=%s",
             qr ? "Response" : "Query", qname_off > 0 ? qname : "(root)");
    return 1;
}

/* ---- TLS ClientHello, RFC 8446: TLS record header (content type 0x16 =
 * Handshake, version, length) followed by a Handshake header (type 0x01
 * = ClientHello). We also parse the extensions to pull out SNI (Server
 * Name Indication, extension type 0x0000) ourselves, entirely by hand --
 * this is the same field nDPI exposes as host_server_name for TLS, but
 * here it's extracted with zero external library involvement. ---- */
static int match_tls_clienthello(const uint8_t *payload, uint16_t len,
                                  char *detail, size_t detail_sz)
{
    if (payload == NULL || len < 43)
        return 0;

    if (payload[0] != 0x16)             /* content type: Handshake */
        return 0;
    if (payload[1] != 0x03)             /* major version: TLS (SSLv3-family) */
        return 0;
    if (payload[5] != 0x01)             /* handshake type: ClientHello */
        return 0;

    /* Walk past: handshake header(4) + client_version(2) + random(32) */
    const uint8_t *p = payload + 5 + 4 + 2 + 32;
    const uint8_t *end = payload + len;
    if (p >= end) {
        snprintf(detail, detail_sz, "ClientHello (truncated, no SNI parsed)");
        return 1;
    }

    uint8_t session_id_len = *p++;
    if (p + session_id_len > end) goto no_sni;
    p += session_id_len;

    if (p + 2 > end) goto no_sni;
    uint16_t cipher_suites_len = (p[0] << 8) | p[1];
    p += 2;
    if (p + cipher_suites_len > end) goto no_sni;
    p += cipher_suites_len;

    if (p + 1 > end) goto no_sni;
    uint8_t compression_len = *p++;
    if (p + compression_len > end) goto no_sni;
    p += compression_len;

    if (p + 2 > end) goto no_sni;
    uint16_t extensions_len = (p[0] << 8) | p[1];
    p += 2;
    const uint8_t *ext_end = p + extensions_len;
    if (ext_end > end) ext_end = end;

    while (p + 4 <= ext_end) {
        uint16_t ext_type = (p[0] << 8) | p[1];
        uint16_t ext_len  = (p[2] << 8) | p[3];
        p += 4;
        if (p + ext_len > ext_end)
            break;

        if (ext_type == 0x0000 && ext_len >= 5) {
            /* server_name extension: list_len(2) name_type(1) name_len(2) name */
            const uint8_t *sni = p + 2 + 1 + 2;
            uint16_t name_len = (p[3] << 8) | p[4];
            if (sni + name_len <= ext_end && name_len < 256) {
                char sni_buf[256];
                memcpy(sni_buf, sni, name_len);
                sni_buf[name_len] = '\0';
                snprintf(detail, detail_sz, "ClientHello SNI=%s", sni_buf);
                return 1;
            }
        }
        p += ext_len;
    }

no_sni:
    snprintf(detail, detail_sz, "ClientHello (no SNI extension found)");
    return 1;
}

/* [ADD MORE MATCHERS HERE] -- same signature, then register below. This
 * is the entire extension point for "teach this program a new format
 * it didn't know before": no database, no library call, just a function
 * that checks bytes against a spec you provide. */
struct custom_protocol_matcher {
    const char *name;
    proto_matcher_fn match;
};

static struct custom_protocol_matcher matchers[] = {
    { "HTTP-Request",    match_http_request },
    { "HTTP-Response",   match_http_response },
    { "TLS-ClientHello", match_tls_clienthello },
    { "DNS",             match_dns },
};
#define N_MATCHERS (sizeof(matchers) / sizeof(matchers[0]))

/* Runs every registered matcher in order; first structural match wins.
 * If NONE match, the traffic is UNKNOWN -- genuinely, to this program,
 * not "unknown to some external database" -- there is no such database
 * here at all. */
static const char *custom_classify(const uint8_t *payload, uint16_t len,
                                    char *detail, size_t detail_sz)
{
    detail[0] = '\0';
    for (size_t i = 0; i < N_MATCHERS; i++) {
        if (matchers[i].match(payload, len, detail, detail_sz))
            return matchers[i].name;
    }
    return "UNKNOWN";
}

/* [ADDED] Shannon entropy -- pure math, no protocol knowledge, works
 * identically regardless of whether custom_classify() found a match. */
static double shannon_entropy(const uint8_t *data, uint16_t len)
{
    if (len == 0)
        return 0.0;
    uint32_t counts[256] = {0};
    for (uint16_t i = 0; i < len; i++)
        counts[data[i]]++;
    double entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] == 0) continue;
        double p = (double)counts[i] / (double)len;
        entropy -= p * (log(p) / log(2.0));
    }
    return entropy;
}

/* ============================================================
 * Flow tracking -- same 5-tuple hash-table idea as before, but the
 * "state" kept per flow is now just our own classification string,
 * not an nDPI flow object (there is no such object in this file).
 * ============================================================ */
struct flow_key {
    uint32_t ip_a, ip_b;
    uint16_t port_a, port_b;
    uint8_t l4_proto;
};

struct flow_entry {
    int valid;
    struct flow_key key;
    uint64_t last_seen_ms;
    char last_proto_name[32];
    char last_detail[MATCH_DETAIL_MAX];
};

static struct flow_entry flow_table[FLOW_TABLE_SIZE];

static uint64_t now_ms(void)
{
    return (rte_get_timer_cycles() * 1000ULL) / rte_get_timer_hz();
}

static uint32_t flow_hash(const struct flow_key *k)
{
    uint32_t h = 2166136261u;
    h ^= k->ip_a;   h *= 16777619u;
    h ^= k->ip_b;   h *= 16777619u;
    h ^= k->port_a; h *= 16777619u;
    h ^= k->port_b; h *= 16777619u;
    h ^= k->l4_proto; h *= 16777619u;
    return h;
}

static void flow_make_key(uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port,
                           uint8_t l4_proto, struct flow_key *key)
{
    if (src_ip < dst_ip || (src_ip == dst_ip && src_port <= dst_port)) {
        key->ip_a = src_ip; key->port_a = src_port;
        key->ip_b = dst_ip; key->port_b = dst_port;
    } else {
        key->ip_a = dst_ip; key->port_a = dst_port;
        key->ip_b = src_ip; key->port_b = src_port;
    }
    key->l4_proto = l4_proto;
}

static int flow_key_equal(const struct flow_key *a, const struct flow_key *b)
{
    return a->ip_a == b->ip_a && a->ip_b == b->ip_b &&
           a->port_a == b->port_a && a->port_b == b->port_b &&
           a->l4_proto == b->l4_proto;
}

static struct flow_entry *flow_get(const struct flow_key *key, uint64_t ts)
{
    uint32_t start = flow_hash(key) % FLOW_TABLE_SIZE;
    struct flow_entry *oldest = NULL;

    for (uint32_t n = 0; n < FLOW_TABLE_SIZE; n++) {
        uint32_t pos = (start + n) % FLOW_TABLE_SIZE;
        struct flow_entry *e = &flow_table[pos];

        if (!e->valid) {
            e->valid = 1;
            e->key = *key;
            e->last_seen_ms = ts;
            e->last_proto_name[0] = '\0';
            e->last_detail[0] = '\0';
            return e;
        }
        if (flow_key_equal(&e->key, key)) {
            e->last_seen_ms = ts;
            return e;
        }
        if (oldest == NULL || e->last_seen_ms < oldest->last_seen_ms)
            oldest = e;
    }

    if (oldest != NULL) {
        oldest->key = *key;
        oldest->last_seen_ms = ts;
        oldest->last_proto_name[0] = '\0';
        oldest->last_detail[0] = '\0';
        return oldest;
    }
    return NULL;
}

/* [ADDED] Full IPv4/TCP/UDP header dump + hex/ASCII payload preview.
 * Same idea as the previous version, but "protocol=" now reports what
 * OUR OWN matcher concluded, with no nDPI category/risk fields (those
 * were nDPI-specific and don't exist without it). */
#define PAYLOAD_DUMP_MAX 64

static void hex_dump(const uint8_t *data, uint16_t len)
{
    uint16_t shown = (len < PAYLOAD_DUMP_MAX) ? len : PAYLOAD_DUMP_MAX;
    for (uint16_t off = 0; off < shown; off += 16) {
        printf("    %04u  ", off);
        for (uint16_t i = 0; i < 16; i++) {
            if (off + i < shown) printf("%02X ", data[off + i]);
            else printf("   ");
        }
        printf(" ");
        for (uint16_t i = 0; i < 16 && off + i < shown; i++) {
            uint8_t b = data[off + i];
            printf("%c", (b >= 32 && b < 127) ? b : '.');
        }
        printf("\n");
    }
    if (len > PAYLOAD_DUMP_MAX)
        printf("    ... (%u more bytes not shown)\n", len - PAYLOAD_DUMP_MAX);
}

static void print_packet_detail(struct rte_ipv4_hdr *ip, uint8_t l4_proto,
                                 const void *l4_hdr, const char *proto_name,
                                 const char *detail,
                                 const char *sstr, const char *dstr,
                                 uint16_t src_port, uint16_t dst_port,
                                 const uint8_t *payload, uint16_t payload_len)
{
    uint8_t ihl = (ip->version_ihl & 0x0F) * 4;
    uint16_t total_len = rte_be_to_cpu_16(ip->total_length);

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

    printf("  -- Classification (hand-written grammar match, NO external\n");
    printf("     signature database consulted) -----------------------\n");
    printf("    protocol=%s%s%s\n", proto_name,
           detail[0] ? "  detail=" : "", detail);

    if (payload_len > 0) {
        double entropy = shannon_entropy(payload, payload_len);
        printf("  -- Payload (%u bytes, showing up to %d) ------------------\n",
               payload_len, PAYLOAD_DUMP_MAX);
        printf("    entropy=%.2f / 8.0  %s\n", entropy,
               entropy > 7.5 ? "(HIGH -- consistent with encrypted/packed/random data)" :
               entropy > 6.0 ? "(moderate)" : "(low -- looks like structured/plaintext data)");
        hex_dump(payload, payload_len);
    } else {
        printf("  -- Payload: none (header-only packet, e.g. bare SYN/ACK) --\n");
    }
    printf("  -----------------------------------------------------------\n\n");
}

/* [ADDED] The whole "DPI" step, with zero external library involvement.
 * Every packet's classification result comes ONLY from custom_classify()
 * above, which itself only ever consults the matchers[] array you wrote
 * by hand. There is no fallback to any signature database anywhere. */
static void classify_ipv4(struct rte_mbuf *mbuf, struct rte_ipv4_hdr *ip)
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

    uint16_t src_port = 0, dst_port = 0;
    uint8_t l4_hdr_len = 0;
    void *l4_hdr = (uint8_t *)ip + ihl;

    if (l4_proto == IPPROTO_TCP) {
        struct rte_tcp_hdr *tcp = l4_hdr;
        src_port = rte_be_to_cpu_16(tcp->src_port);
        dst_port = rte_be_to_cpu_16(tcp->dst_port);
        l4_hdr_len = (tcp->data_off >> 4) * 4;
    } else {
        struct rte_udp_hdr *udp = l4_hdr;
        src_port = rte_be_to_cpu_16(udp->src_port);
        dst_port = rte_be_to_cpu_16(udp->dst_port);
        l4_hdr_len = sizeof(struct rte_udp_hdr);
    }

    uint32_t src_ip = rte_be_to_cpu_32(ip->src_addr);
    uint32_t dst_ip = rte_be_to_cpu_32(ip->dst_addr);

    const uint8_t *payload = (const uint8_t *)l4_hdr + l4_hdr_len;
    uint16_t payload_len = (total_len > (uint16_t)(ihl + l4_hdr_len)) ?
        (uint16_t)(total_len - ihl - l4_hdr_len) : 0;

    struct flow_key key;
    flow_make_key(src_ip, src_port, dst_ip, dst_port, l4_proto, &key);
    struct flow_entry *entry = flow_get(&key, now_ms());
    if (entry == NULL)
        return;

    char detail[MATCH_DETAIL_MAX];
    const char *proto_name = custom_classify(payload, payload_len, detail, sizeof(detail));

    /* Only print when the classification actually changed for this flow --
     * same discipline as before, so this stays cheap at line rate. */
    if (strcmp(proto_name, entry->last_proto_name) != 0 ||
        strcmp(detail, entry->last_detail) != 0) {

        char sstr[16], dstr[16];
        ip_to_str(src_ip, sstr, sizeof(sstr));
        ip_to_str(dst_ip, dstr, sizeof(dstr));

        if (strcmp(proto_name, "UNKNOWN") == 0) {
            printf("[UNKNOWN-FLAG] No hand-written format matched this "
                   "traffic | %s:%u -> %s:%u (proto %u)\n",
                   sstr, src_port, dstr, dst_port, l4_proto);
        } else {
            printf("[MATCH] %s%s%s | %s:%u -> %s:%u\n",
                   proto_name, detail[0] ? " | " : "", detail,
                   sstr, src_port, dstr, dst_port);
        }

        print_packet_detail(ip, l4_proto, l4_hdr, proto_name, detail,
                             sstr, dstr, src_port, dst_port,
                             payload, payload_len);

        snprintf(entry->last_proto_name, sizeof(entry->last_proto_name), "%s", proto_name);
        snprintf(entry->last_detail, sizeof(entry->last_detail), "%s", detail);
    }
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
        uint64_t rx_miss  = cur[i].imissed  - prev[i].imissed;
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
                   " incoming packets -- RX ring filled up.\n", i, rx_miss);
    }

    prev[0] = cur[0];
    prev[1] = cur[1];
    last_cycles = now;
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
    ip->time_to_live = 64;
    ip->hdr_checksum = 0;
    ip->hdr_checksum = rte_ipv4_cksum(ip);

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

    /* [CHANGED] No nDPI call here anymore -- classify_ipv4() uses only
     * the hand-written matchers[] above. */
    classify_ipv4(mbuf, ip);

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

    ret = rte_pdump_init();
    if (ret < 0)
        printf("[WARN] rte_pdump_init() failed (%d)\n", ret);

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
    printf("  DPDK ROUTER (NO nDPI -- hand-written format matching only)\n");
    printf("============================================================\n");
    printf("Port 0 (VM2 side): IP 192.168.100.1  MAC %s\n", m0);
    printf("Port 1 (VM1 side): IP 192.168.200.1  MAC %s\n", m1);
    printf("Default gateway for non-local traffic: %s (VM1)\n", gw_str);
    printf("============================================================\n");
    printf("[INFO] %zu custom format matchers registered: ", N_MATCHERS);
    for (size_t i = 0; i < N_MATCHERS; i++)
        printf("%s%s", matchers[i].name, (i + 1 < N_MATCHERS) ? ", " : "\n");
    printf("[INFO] Anything not matching one of these is UNKNOWN --\n");
    printf("[INFO] there is no signature database to fall back on.\n");
    printf("[INFO] Press Ctrl+C to stop\n\n");

    while (running) {
        process_port(PORT_VM2);
        process_port(PORT_VM1);
        print_stats();
    }

    rte_pdump_uninit();
    printf("\n[INFO] Stopping ports...\n");
    rte_eth_dev_stop(PORT_VM2);
    rte_eth_dev_stop(PORT_VM1);
    rte_eth_dev_close(PORT_VM2);
    rte_eth_dev_close(PORT_VM1);

    return 0;
}
jetson_router_noNdpi.c
Displaying jetson_router_noNdpi.c.
