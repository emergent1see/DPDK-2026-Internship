# Building the DPDK Layer-3 Router (Access the internet)

This is our own record of building a real Layer-3 router with DPDK, from the point where
we had a working two-VM packet relay all the way through to a genuine IP router that
correctly forwards traffic between two separate subnets. We're writing this the way it
actually happened, including the wrong turns, so we have a real record of the reasoning,
not just the final clean version of the code.

## Where we started

We already had a working environment: DPDK built and running on an Ubuntu Server 24.04
VM, two NICs bound to `vfio-pci`, hugepages configured, the whole thing polling packets
successfully. On top of that we had a simple two-port forwarder, an application that
just took whatever frame arrived on Port 0 and blindly sent it out Port 1, and vice
versa.

The lab setup was three VMs: VM2 and VM1 as the two endpoints, and the middle VM running
our DPDK app between them. The goal was simple to state: ping from VM2 to VM1 and get a
reply back. It turned out to not be simple at all.

## Stage 1: the relay that didn't work

Our first version looked roughly like this:

```c
static void forward_packet(struct rte_mbuf *mbuf, uint16_t from_port, uint16_t to_port)
{
    rte_eth_tx_burst(to_port, 0, &mbuf, 1);
}
```

Dead simple. Whatever comes in one port goes out the other, completely untouched.

We pinged VM1 from VM2 and got nothing back. Not even a "destination unreachable," just
silence. `ping` sat there with request timed out over and over.

## Stage 2: chasing the wrong theory

Our first assumption was that this had to be an Ethernet-level problem, something about
the destination MAC or the way we were forwarding frames. We went down the path of
thinking maybe the receiving VM's kernel was rejecting the ICMP reply because the
Ethernet source MAC on the frame didn't match what it expected.

That theory turned out to be wrong, and it's worth writing down why, because it's a
genuinely common misconception. Linux (and most OS network stacks) match an ICMP echo
reply purely on IP source/destination address plus the ICMP identifier and sequence
number. It does not care what the Ethernet source MAC on the frame was. So chasing "the
kernel is rejecting the MAC" as a theory was a dead end from the start.

What actually *is* a real concern at this layer, and what we checked next, is
hypervisor-level filtering. VirtualBox (and VMware, and KVM with macvtap) will silently
drop a frame at the virtual switch level if the Ethernet source MAC doesn't match the
vNIC's real MAC, this is the "forged transmit" / anti-spoofing protection every
hypervisor has by default. If our DPDK app forwarded a frame out Port 1 while leaving
VM2's original source MAC untouched, the hypervisor could be dropping it before it ever
reached VM1, invisibly, with nothing showing up in `tcpdump` on either guest because the
frame never got that far.

We set Promiscuous Mode to "Allow All" on both DPDK-facing adapters (not just "Allow
VMs", that setting alone isn't enough) to rule this out as a factor.

## Stage 3: building a proper debugging checklist

Rather than keep guessing, we forced ourselves to actually localize where the packet was
dying, in order:

1. `tcpdump -i <if> icmp` on VM1, does the echo request even arrive? Does VM1 send a
   reply out at all?
2. Since DPDK bypasses the kernel entirely once a NIC is bound to `vfio-pci`, a normal
   `tcpdump` on the DPDK host's interfaces shows nothing, the kernel doesn't see that
   traffic anymore. So instead we needed RX/TX debug counters printed directly from
   inside the DPDK app itself.
3. Confirm Port1 RX increments the moment VM1 sends its reply.
4. Confirm Port0 TX increments right after, meaning our forwarder actually called
   `rte_eth_tx_burst()` and it returned 1 (not 0, which would mean the TX ring was full
   or the driver rejected it).
5. Check hypervisor promiscuous/forged-transmit settings again, just to be sure.
6. `tcpdump -i <if> icmp` on VM2, does the reply frame physically land on the wire at
   all, even if `ping` itself never reports it?

This systematic approach is what actually cracked the problem open, not more theorizing
about MAC addresses.

## Stage 4: the real root cause

We took a screenshot of both VMs' IP configuration and finally looked at it properly:

- VM1's `enp0s8` was `192.168.200.2/24`
- VM2's `enp0s8` was `192.168.100.2/24`

These are two completely different subnets. VM1 had a route configured like:

```
192.168.100.0/24 via 192.168.200.1 dev enp0s8
```

And there was our actual bug, sitting in plain sight the whole time. `192.168.200.1` was
a gateway IP that didn't belong to any real host on the wire. Nothing was ever going to
answer ARP for it, which meant that route could never resolve, full stop. Every time
we'd tried `arp -s 192.168.200.1 <some-mac>` to force a static entry, it failed with
`SIOCSARP: Network is unreachable`, and now we understood exactly why: we were trying to
statically map an IP that wasn't even part of VM1's local subnet in the first place.
That error message wasn't a fluke or a permissions issue, it was Linux correctly telling
us the request made no sense.

Our DPDK app at this stage was a pure Layer-2 frame relay. It had no IP stack at all. It
didn't answer ARP, it didn't route, it didn't touch TTL or checksums. That's completely
fine *if* both VMs believe they're on the same L2 segment. But we had configured the VMs
as if there were a real router sitting between two separate subnets, and nothing was
actually playing that role. The relay was moving frames, but nobody was doing the job
of being a gateway.

This was the moment the actual scope of the fix became clear. We had two real options.

**Option A** would have been the fast path: put both VMs on the same subnet
(`192.168.50.0/24` for both, no gateway needed at all), let ARP resolve directly between
their real MACs, and keep the DPDK app as a pure pass-through with zero MAC rewriting.
That would have gotten ping working in about five minutes.

We didn't want that. The whole point of this project for us was to actually build real
DPDK router behavior, not fake it by hiding the problem. So we went with **Option B**:
keep the two separate subnets, and make the DPDK app genuinely act like an IP router.

## Stage 5: designing the router properly

Once we committed to Option B, the design fell out fairly naturally from what an actual
router has to do:

```
VM2 (192.168.100.2/24)  <-->  [Port0: 192.168.100.1]  DPDK  [Port1: 192.168.200.1]  <-->  VM1 (192.168.200.2/24)
```

`192.168.100.1` and `192.168.200.1` needed to become real, ARP-able IP addresses owned
directly by our DPDK application, one per port. VM2's route table would say "reach
192.168.200.0/24 via 192.168.100.1", and this time that gateway IP would actually mean
something because our own code would be the one answering ARP for it.

VM configuration changed to:

**VM1:**
```bash
sudo ip addr flush dev enp0s8
sudo ip addr add 192.168.200.2/24 dev enp0s8
sudo ip route add 192.168.100.0/24 via 192.168.200.1 dev enp0s8
```

**VM2:**
```bash
sudo ip addr flush dev enp0s8
sudo ip addr add 192.168.100.2/24 dev enp0s8
sudo ip route add 192.168.200.0/24 via 192.168.100.1 dev enp0s8
```

And critically: no static ARP entries anywhere. The whole point was for the router to
answer ARP dynamically, the way a real gateway does.

## Stage 6: building the router, piece by piece

### Giving each port an identity

The first thing the old relay was missing was any concept of "this port has an IP
address." We added:

```c
struct router_iface {
    uint16_t         port_id;
    uint32_t         ip;
    uint32_t         subnet;
    uint32_t         netmask;
    struct rte_ether_addr mac;
};
static struct router_iface ifaces[2];
```

and populated it at startup with the two gateway IPs, `255.255.255.0` netmasks, and each
port's real MAC pulled via `rte_eth_macaddr_get()`. This alone was the conceptual
turning point, the app was no longer just two wires, each port was now a real network
interface with its own identity.

### Making the router answer ARP for itself

Without this, we'd be right back where we started, VM2 asking "who has 192.168.100.1?"
into the void forever. We wrote `handle_arp()`:

```c
static void handle_arp(struct rte_mbuf *mbuf, uint16_t port_id)
{
    uint32_t sender_ip = rte_be_to_cpu_32(arp->arp_data.arp_sip);
    uint32_t target_ip = rte_be_to_cpu_32(arp->arp_data.arp_tip);
    uint16_t opcode = rte_be_to_cpu_16(arp->arp_opcode);

    arp_learn(port_id, sender_ip, &arp->arp_data.arp_sha);

    if (opcode == RTE_ARP_OP_REQUEST && target_ip == iface->ip) {
        send_arp_reply(mbuf, port_id);
    }

    rte_pktmbuf_free(mbuf);
}
```

We made a deliberate choice here to always learn the sender's IP/MAC regardless of
whether the packet was a request or a reply, and regardless of whether it targeted this
router's own IP. Only the reply itself is conditional on both the opcode and the target
IP matching. This means the router builds up its knowledge of VM1 and VM2's real
addresses passively, just from watching traffic go by, which is exactly how a real ARP
cache behaves.

### Building the actual ARP cache

```c
struct arp_entry { uint32_t ip; struct rte_ether_addr mac; int valid; };
static struct arp_entry arp_table[2][ARP_TABLE_SIZE];
```

One tiny table per port. `arp_learn()` does a linear scan, updates an existing entry if
the IP is already known (only logging if the MAC actually changed, otherwise every
single packet would spam a log line), or inserts into the first free slot it saw during
the same pass. We capped this at 16 slots per port with the simplest possible eviction
policy (overwrite slot 0 if full), which is obviously not production-grade, but given
this lab only ever has exactly one real host per port, it was the right amount of
complexity for what we actually needed right now.

### The forwarding decision

This is where the router actually starts behaving differently from the old relay:

```c
if ((dst_ip & ifaces[0].netmask) == ifaces[0].subnet) {
    out_port = ifaces[0].port_id;
} else if ((dst_ip & ifaces[1].netmask) == ifaces[1].subnet) {
    out_port = ifaces[1].port_id;
} else {
    // no route, drop
}
```

The old relay's forwarding logic was "whatever comes in Port 0 goes out Port 1," full
stop, no IP awareness whatsoever. This is a real routing table lookup, checking the
destination IP against each interface's subnet with a masked comparison. Small in scale
(only two routes), but conceptually identical to what a much bigger router does.

### Resolving the next hop, and hitting our first "expected" failure

```c
if (!arp_lookup(out_port, dst_ip, &next_hop_mac)) {
    send_arp_request(pool, out_port, dst_ip);
    rte_pktmbuf_free(mbuf);
    return;
}
```

The first time we tested this, our very first ping dropped. We actually paused and
thought we'd broken something, until we looked at the console output:

```
[ARP MISS] Don't know MAC for 192.168.200.2 yet -- requesting, dropping this packet
[ARP] Sent request on port 1 for 192.168.200.2
[ARP] Learned 192.168.200.2 -> <mac> on port 1
```

This wasn't a bug. This is exactly what a real router or a real Linux box does on a cold
ARP cache: it can't legitimately construct an Ethernet frame without knowing the
destination MAC, so it asks, and drops the packet that triggered the question. The
sending host's own TCP/IP stack retransmits shortly after (this is why the very first
ping or the very first TCP SYN of a connection sometimes shows a visible ~1 second
delay), and by the time it does, the ARP entry exists and everything after that is
immediate. Recognizing this as correct behavior rather than a bug was an important
moment, it meant we understood *why* it was happening instead of just seeing a dropped
packet and panicking.

### Making the router actually behave like a router, not just a smarter relay

The last piece was the part that a bridge is specifically *not* allowed to do:

```c
if (ip->time_to_live <= 1) {
    // drop, TTL expired
}
ip->time_to_live--;
ip->hdr_checksum = 0;
ip->hdr_checksum = rte_ipv4_cksum(ip);

rte_ether_addr_copy(&out_iface->mac, &eth->src_addr);
rte_ether_addr_copy(&next_hop_mac, &eth->dst_addr);
```

We decrement TTL and recompute the IPv4 header checksum on every forwarded packet, this
is mandatory router behavior (it's literally the mechanism `traceroute` exploits), and a
pure L2 bridge must never do this. Then we rewrite the Ethernet header: source becomes
this router's own outgoing port MAC, destination becomes the resolved next hop's real
MAC. This was the moment we actually understood, not just intellectually but from having
built it, why L2 addressing changes at every router hop while L3 addressing (the IP
addresses) stays constant end to end. We'd read that fact before, but writing the code
that does it is what made it click.

One thing we specifically checked and decided *not* to touch: TCP and UDP checksums.
Those cover a pseudo-header of source/destination IP plus the payload, and since this
router never changes IP addresses (only TTL, which isn't part of that pseudo-header),
those checksums stay valid without any recomputation on our end. If we ever add NAT to
this later, that assumption breaks and we'd have to recompute those too, but for plain
routing it was correct to leave them alone.

## Stage 7: testing, for real this time

### ICMP

```
ping 192.168.200.2   (from VM2)
```

First packet dropped exactly as expected (ARP miss), every ping after that got a clean
reply. Router console:

```
[ROUTE] ICMP: 192.168.100.2 -> 192.168.200.2 | in=port0 out=port1
[ROUTE] ICMP: 192.168.200.2 -> 192.168.100.2 | in=port1 out=port0
```

Seeing that symmetric pair of ROUTE lines for the first time, request going one way,
reply coming back the other, was the actual "it works" moment for this whole stage of
the project.

### Confirming it wasn't just ICMP

We wanted to know if this was really a general-purpose router or something that happened
to only work for ping. We tested with plain `nc`:

VM1: `nc -l -p 5000`
VM2: `nc 192.168.200.2 5000`

Typed text on one side, watched it appear on the other, and watched the router log every
segment of the exchange as ordinary `TCP` traffic. That confirmed something important
about how `handle_ipv4()` is written: it only ever looks at the IP header (source, dest,
TTL, checksum) to make its decision. Whatever comes after that header, TCP, UDP, ICMP,
raw bytes, gets carried through completely untouched as part of the same mbuf. The
`proto` string we print in the log line (`ICMP`/`TCP`/`UDP`/`OTHER`) is purely cosmetic,
it doesn't gate or change forwarding behavior at all.

### Building a real client/server test

To have something more deterministic than typing into `nc`, we wrote a minimal TCP
client/server pair that just exchanges "Hi" and "Hello". Running the server on VM1
(plain POSIX sockets, nothing DPDK-specific, it's just an ordinary program on the VM's
normal kernel network stack) and the client on VM2, we got:

```
[CLIENT] Connecting to 192.168.200.2:5000...
[CLIENT] Connected!
[CLIENT] Sent: Hi
[CLIENT] Received: Hello
```

with the full three-way handshake, the data segments, and the FIN/ACK teardown all
visible in the router's log:

```
[ROUTE] TCP: 192.168.100.2 -> 192.168.200.2 | in=port0 out=port1   (SYN)
[ROUTE] TCP: 192.168.200.2 -> 192.168.100.2 | in=port1 out=port0   (SYN-ACK)
[ROUTE] TCP: 192.168.100.2 -> 192.168.200.2 | in=port0 out=port1   (ACK)
[ROUTE] TCP: 192.168.100.2 -> 192.168.200.2 | in=port0 out=port1   ("Hi")
[ROUTE] TCP: 192.168.200.2 -> 192.168.100.2 | in=port1 out=port0   ("Hello")
```

This felt like the real proof point for the whole exercise: two completely ordinary
socket applications, with no awareness that DPDK exists, successfully talking across two
different subnets purely because of the router sitting in between doing its job
correctly.

## What we got wrong along the way, summarized

Writing this out honestly:

1. We first assumed the problem was at the Ethernet/MAC layer (the kernel "rejecting" a
   reply based on source MAC), which was flatly wrong, Linux doesn't validate ICMP
   replies that way. We spent time on this before ruling it out properly.
2. We didn't have a real debugging checklist at first, we were pattern-matching to
   plausible-sounding theories instead of actually localizing where the packet died. The
   moment we forced ourselves into "check request arrival, check RX counters, check TX
   counters, check hypervisor settings, check reply on the wire," in that specific
   order, the real cause fell out almost immediately.
3. The actual root cause, two subnets with a gateway IP nobody owned, was visible in our
   own VM IP configuration the entire time. We just hadn't looked at it as a topology
   problem, we'd been looking at it as a DPDK forwarding bug.
4. Our `arp -s` attempts were never going to work, and the "Network is unreachable"
   error was Linux being correct, not a symptom of something broken in our setup.

## Where this leaves us

The router works correctly for ICMP and TCP across two real subnets, with proper ARP
resolution, TTL/checksum handling, and per-hop MAC rewriting. Known rough edges we're
aware of and plan to address together next:

- The ARP cache is a fixed 16-slot array per port with the crudest possible eviction, it
  would need a real hash table or LRU to handle more than one host per side.
- No proactive ARP on startup, so the first packet in each direction always eats one
  ARP-miss drop. Fine for understanding the mechanism, not ideal for a production feel.
- No ICMP error generation, TTL expiry and no-route cases just silently drop instead of
  sending back `Time Exceeded` or `Destination Unreachable` the way a standards-compliant
  router would.
- No fragmentation handling, currently assumes both VMs stay at the default 1500-byte
  MTU.

Next step we're considering: comparing this against DPDK's own `l3fwd` reference sample
application, to see how a production-grade implementation handles the same problems
(especially the ARP cache and the missing ICMP error generation) differently from what
we built here.
