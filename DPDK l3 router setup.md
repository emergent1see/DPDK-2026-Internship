# DPDK Layer-3 Router: Full Setup & Debugging Journal

This document covers the complete journey of building a three-VM DPDK packet-forwarding
lab, from environment setup, through a broken Layer-2 relay, to a working Layer-3 router
that correctly forwards ICMP/TCP/UDP traffic between two separate subnets.

It's written as an internship journal: what I built, what broke, why it broke, and how I
fixed it, in the order it actually happened, not the "clean" order.

## 1. Goal

Three VMs, one virtual network in between:

```
VM2 (192.168.100.2/24)  <---->  [ DPDK middlebox ]  <---->  VM1 (192.168.200.2/24)
                          Port0                     Port1
```

The middlebox VM runs a DPDK application with two NIC ports. Traffic from VM2 should be
able to reach VM1 and vice versa, bidirectionally, with the DPDK app doing the actual
forwarding work in userspace (kernel bypass), not the OS network stack.

The end goal was simple to state and hard to get right. `ping` between VM1 and VM2
should work, and so should ordinary TCP/UDP application traffic (verified with a raw
socket client/server and `nc`).

## 2. Environment Setup (the middlebox VM)

### 2.1 First attempt: Oracle Linux in VirtualBox, lost the virtual disk

The initial plan was Oracle Linux under VirtualBox. To get IOMMU emulation working
(needed for `vfio-pci`), the VM's chipset had to be changed from PIIX3 to ICH9.

**What broke:** changing the chipset caused the VM to lose its attached virtual disk.
VirtualBox no longer saw the existing `.vdi` after the chipset swap, effectively
orphaning the whole install.

**Resolution:** rather than fight VirtualBox's chipset/disk-controller mismatch, I
rebuilt the environment from scratch on a different distro (see below). Lesson learned:
set the chipset to ICH9 before installing the OS, not after. Changing it later can
silently break the disk controller mapping.

### 2.2 Second attempt: Ubuntu Server 24.04 LTS in VirtualBox

This became the environment that actually worked. Setup sequence:

1. **Create VM with ICH9 chipset from the start** (Settings, System, Motherboard/Chipset).
2. **Enable IOMMU emulation.** This required running `VBoxManage modifyvm <vm> --iommu=intel`
   separately from setting the chipset, setting the chipset alone was not sufficient.

   **Error encountered:** even with ICH9 selected, `/sys/kernel/iommu_groups/` was empty
   (zero IOMMU groups) inside the guest, meaning `vfio-pci` binding was impossible.

   **Resolution:** `VBoxManage modifyvm <vmname> --iommu=intel` had to be run explicitly
   from the host, on a powered-off VM. The GUI chipset setting alone does not enable IOMMU
   emulation, that's a separate flag entirely.

3. **Hugepages.** Configured 2MB hugepages (mounted at `/dev/hugepages`), required
   because DPDK's `rte_mempool`/`mbuf` allocations rely on hugepage-backed memory to
   avoid TLB thrashing at line rate.

4. **Load `vfio-pci` / `vfio` kernel modules** (`modprobe vfio vfio-pci`, and
   `vfio_iommu_type1` with `allow_unsafe_interrupts=1` since this is a nested/virtualized
   IOMMU without full interrupt remapping support).

5. **Build DPDK from source.**

   **Error encountered:** the build was OOM-killed partway through. The VM's default
   RAM/no-swap configuration wasn't enough for a parallel Meson/Ninja build.

   **Resolution:**
   - Added a swapfile to the VM.
   - Limited build parallelism with `ninja -j2` instead of the default (which tries to
     use all cores and blows up memory usage on a small VM).

6. **Bind a NIC to `vfio-pci`.**

   **Error encountered #1:** `dpdk-devbind.py` refused to bind an interface that still
   had an active route/IP assigned to it.

   **Resolution:** bring the interface down and strip its IP first
   (`ip addr flush dev <if>`, `ip link set <if> down`) before binding.

   **Error encountered #2:** accidentally bound the NAT adapter (the VM's
   internet-facing interface) instead of the internal/host-only adapter meant for DPDK.
   This immediately cut off the VM's internet access and had to be undone by re-binding
   that interface back to its kernel driver (`dpdk-devbind.py -u`, then re-bind to the
   original driver, e.g. `e1000`).

   **Resolution:** always double-check `dpdk-devbind.py --status` output and match by PCI
   address, not by assumption/ordering. VirtualBox doesn't guarantee NIC enumeration
   order matches the adapter slot order shown in the GUI.

7. **Link status showing DOWN on emulated e1000 NICs.**

   **Error encountered:** even after binding correctly, DPDK reported link status DOWN,
   so no packets flowed.

   **Resolution:** VirtualBox's emulated e1000 NIC doesn't always raise link-up in time
   for DPDK's default link-check on init. Fixed with `--disable-link-check` at EAL init,
   combined with a short settle-delay + retry loop in application startup code that
   re-polls `rte_eth_link_get()` a few times before giving up.

## 3. Network Topology & VM IP Configuration

### 3.1 First working topology (single subnet, pure L2 bridge)

Before attempting cross-subnet routing, I validated the DPDK forwarding path itself with
both VMs on the same subnet, so ARP could resolve directly between the two guests with
no gateway involved:

**VM1:**
```bash
sudo ip addr flush dev enp0s8
sudo ip addr add 192.168.50.2/24 dev enp0s8
```

**VM2:**
```bash
sudo ip addr flush dev enp0s8
sudo ip addr add 192.168.50.3/24 dev enp0s8
```

With both VMs on `192.168.50.0/24`, ARP broadcasts pass through the DPDK relay untouched
and each side learns the other's real MAC directly, no MAC rewriting needed, no gateway
IP needed. This confirmed the packet-plumbing (mempool, RX/TX rings, EAL setup) was
sound before adding any L3 complexity on top.

### 3.2 Final topology (two subnets, real L3 routing)

This is the configuration used by the final `dpdk_router.c`:

**VM1** (`192.168.200.2/24`, reaches the other subnet via the DPDK box as gateway):
```bash
sudo ip addr flush dev enp0s8
sudo ip addr add 192.168.200.2/24 dev enp0s8
sudo ip route add 192.168.100.0/24 via 192.168.200.1 dev enp0s8
```

**VM2** (`192.168.100.2/24`):
```bash
sudo ip addr flush dev enp0s8
sudo ip addr add 192.168.100.2/24 dev enp0s8
sudo ip route add 192.168.200.0/24 via 192.168.100.1 dev enp0s8
```

Important: no static ARP entries (`arp -s`) are needed or used. The DPDK app answers
ARP for `.1` on each side dynamically, exactly like a real gateway would.

Gateway IPs, owned by the DPDK app itself (not by any real host):

| Interface | Port | IP (gateway role)  | Faces  |
|-----------|------|---------------------|--------|
| Port 0    | 0    | `192.168.100.1`     | VM2    |
| Port 1    | 1    | `192.168.200.1`     | VM1    |

## 4. The Debugging Arc: Why Earlier Versions Failed

### 4.1 Version 1, pure L2 relay

```c
static void forward_packet(struct rte_mbuf *mbuf, uint16_t from_port, uint16_t to_port)
{
    rte_eth_tx_burst(to_port, 0, &mbuf, 1);
}
```

Just shuffles whatever frame arrives on one port out the other port, untouched. Works
only if both VMs believe they're on the same L2 segment (same subnet, no gateway
required), see section 3.1.

**Symptom when subnets differed:** `ping` from VM2 to VM1 produced no response at all.
`arp -s <gatewayIP> <mac>` attempts failed with `SIOCSARP: Network is unreachable`,
because the gateway IP being statically ARPed for wasn't even on the local subnet, a
fundamentally invalid ARP entry, not a DPDK bug.

### 4.2 Version 2, L2 relay with blind MAC rewriting

An intermediate version rewrote the Ethernet source MAC on egress to the outgoing port's
own MAC, to work around VirtualBox's forged-transmit protection (see section 5.2). This
fixed "frame silently dropped by the hypervisor," but did not fix the underlying
problem: the app still had no IP stack, didn't answer ARP, and didn't do any routing. It
was a patch for a symptom, not the actual missing functionality.

### 4.3 Version 3, real Layer-3 router (`dpdk_router.c`, final)

This is the version documented in full in section 6. It actually owns IP addresses,
answers ARP, maintains a MAC cache per interface, makes a real forwarding decision based
on destination subnet, resolves next-hop MACs, and mutates packets (TTL decrement +
checksum recompute) the way a real router hop is required to.

## 5. Errors Encountered During Routing Debug (root causes)

### 5.1 `arp -s` failing with "Network is unreachable"

**Cause:** trying to statically map a gateway IP that didn't belong to the local
subnet's route table at all, a routing configuration error, not a DPDK issue.

**Resolution:** stopped trying to fake ARP entries and instead made the DPDK app the
real owner of the gateway IP, answering ARP for it dynamically (section 6.3).

### 5.2 Hypervisor "forged transmit" / promiscuous filtering

**Cause:** when the DPDK app forwards a frame out a port while leaving the original
source MAC untouched (or using one that doesn't match that vNIC's real MAC), VirtualBox's
virtual switch treats it as MAC spoofing and silently drops it at the hypervisor level.
This happens before the frame ever reaches the peer VM, so it's invisible to `tcpdump`
on either guest.

**Resolution:**
- Set Promiscuous Mode to "Allow All" (not just "Allow VMs") on both DPDK-facing virtual
  adapters, in VirtualBox VM network settings.
- Ensure the Ethernet source MAC on any frame leaving a port matches that port's real
  MAC, which the final router does correctly and legitimately (section 6.6), rather than
  as a spoofing workaround.

### 5.3 Ping request arriving but no reply seen

**Cause:** confirmed via `tcpdump icmp` on the receiving VM that the echo request
arrived, but no reply left the intermediary. Traced to the ARP-resolution gap described
in section 4.1, not a MAC-validation issue on the replying VM's kernel (Linux matches
ICMP echo replies on IP src/dst + ICMP id/seq, not on Ethernet source MAC, so that theory
was ruled out early).

### 5.4 First packet of every "new" connection gets dropped

**Cause:** by design. An ARP-cache miss on the outgoing interface causes the router to
send an ARP request and drop the packet that triggered it (section 6.5), exactly like a
real router or Linux box does on a cold cache.

Not a bug. This shows up as a roughly 1 second stall on the very first `ping` or the very
first TCP `SYN` of a session, while the OS on the sending VM retransmits after its ARP
resolves. Every packet after that is immediate.

## 6. `dpdk_router.c`, Architecture Walkthrough

### 6.1 Per-port identity: `struct router_iface`

```c
struct router_iface {
    uint16_t port_id;
    uint32_t ip;       // host byte order
    uint32_t subnet;
    uint32_t netmask;
    struct rte_ether_addr mac;
};
```

A raw DPDK port has no IP identity by default, the kernel doesn't even know it exists
once it's bound to `vfio-pci`. This struct is what turns "port 0" into "a real gateway
interface with an IP, a subnet, and a MAC," the same conceptual role as a Linux
`ip addr add ... dev enp0s8`.

All IP math (`&` for subnet masking, `==` for comparisons) is done in host byte order
here; conversion to network byte order (`rte_cpu_to_be_32`) happens only when a value is
written into an actual packet header.

### 6.2 Per-port ARP cache

```c
struct arp_entry { uint32_t ip; struct rte_ether_addr mac; int valid; };
static struct arp_entry arp_table[2][ARP_TABLE_SIZE];
```

One small table per port. Populated opportunistically by `arp_learn()`, called from both
ARP handling and IP packet handling, so the cache fills in from ordinary traffic, not
only from explicit ARP exchanges (this mirrors how a real OS ARP cache behaves).

### 6.3 ARP: request, reply, and dispatch

- `send_arp_request()`: builds a broadcast "who has `target_ip`?" frame, used when the
  router needs a next-hop MAC it doesn't have yet.
- `send_arp_reply()`: builds a unicast reply, used only when someone ARPs for an IP this
  router owns (its own gateway IP on that port).
- `handle_arp()`: always learns the sender's IP/MAC first; replies only if the request
  specifically targets this interface's own IP.

### 6.4 Routing decision: `handle_ipv4()`

1. Learn the sender into the ARP cache (non-ARP traffic populates the cache too).
2. Route lookup: `(dst_ip & netmask) == subnet` checked against each interface. With
   only two interfaces this is a linear scan; conceptually it's the same operation a real
   routing table does at much larger scale (longest-prefix match).
3. Loop guard: refuse to forward a packet back out the interface it arrived on.
4. Next-hop MAC resolution. Since both VMs are directly attached (no further hops
   downstream), "next hop" means "the destination host itself." Cache miss means an ARP
   request gets sent and this packet is dropped, letting the sender's OS retransmit.
5. TTL decrement + IPv4 header checksum recompute. This is mandatory router behavior; a
   pure L2 bridge must not do this, but every real routed hop must. The checksum field is
   zeroed before recomputation since it can't include itself in its own calculation.
6. Ethernet header rewrite: source MAC becomes the outgoing port's own MAC, destination
   MAC becomes the resolved next-hop's real MAC. This is the core conceptual shift from
   bridging to routing. At L2 the MAC pair identifies the current hop only and changes at
   every router; at L3 the IP pair identifies the true endpoints and never changes en
   route (unless NAT is involved, which this router does not do).

What deliberately isn't touched: TCP/UDP payload and checksums. Those checksums cover a
pseudo-header of src/dst IP plus payload; since this router never changes IP addresses
(only TTL, which isn't part of that pseudo-header), L4 checksums remain valid without
recomputation.

### 6.5 Packet dispatch: `process_port()`

Pure poll-mode, no interrupts. `rte_eth_rx_burst()` on each port in a tight loop,
dispatch by EtherType (`ARP` goes to `handle_arp`, `IPv4` goes to `handle_ipv4`, anything
else gets freed/dropped). This busy-polling is the fundamental DPDK performance model:
no interrupt latency, no context switches.

## 7. Build & Run

```bash
cd ~/dpdk_router
meson setup build
cd build
ninja
sudo ./dpdk_router -l 0-1 -n 2
```

Expected startup banner confirms both gateway IPs and their real MACs:

```
Port 0 (VM2 side): IP 192.168.100.1  MAC <mac>
Port 1 (VM1 side): IP 192.168.200.1  MAC <mac>
```

## 8. Verification

### 8.1 ICMP

From VM2: `ping 192.168.200.2` (VM1). The first 1 to 2 packets may drop while ARP
resolves (section 5.4 / section 6.4 step 4); subsequent replies should arrive normally.
Console shows:

```
[ARP MISS] Don't know MAC for 192.168.200.2 yet -- requesting, dropping this packet
[ARP] Learned 192.168.200.2 -> <mac> on port 1
[ROUTE] ICMP: 192.168.100.2 -> 192.168.200.2 | in=port0 out=port1
```

### 8.2 Application data (TCP)

Confirmed the router forwards arbitrary application payloads, not just ICMP, using both
`nc` and a small custom client/server:

**Server (VM1):**
```bash
./server 5000
```

**Client (VM2):**
```bash
./client 192.168.200.2 5000
```

Router console shows the full handshake and data segments passing through:
```
[ROUTE] TCP: 192.168.100.2 -> 192.168.200.2 | in=port0 out=port1   (SYN)
[ROUTE] TCP: 192.168.200.2 -> 192.168.100.2 | in=port1 out=port0   (SYN-ACK)
[ROUTE] TCP: 192.168.100.2 -> 192.168.200.2 | in=port0 out=port1   (ACK)
[ROUTE] TCP: 192.168.100.2 -> 192.168.200.2 | in=port0 out=port1   ("Hi")
[ROUTE] TCP: 192.168.200.2 -> 192.168.100.2 | in=port1 out=port0   ("Hello")
```

Confirms the router is protocol-agnostic below L4, it inspects only Ethernet/IP headers
and forwards TCP/UDP/ICMP payloads byte-for-byte, unmodified.

## 9. Known Limitations (not yet addressed)

- ARP table: fixed 16 slots per port, naive eviction (overwrites slot 0 when full). Fine
  for one host per side, would need a proper hash table or LRU for more hosts.
- No proactive ARP on startup, first packet in each direction always triggers one
  ARP-miss drop.
- No ICMP error generation. TTL expiry and no-route cases are silently dropped instead
  of generating `ICMP Time Exceeded` / `Destination Unreachable`, which a
  standards-compliant router would send back to the source.
- No fragmentation/reassembly, assumes both VMs stay at the default 1500-byte MTU.
- No NAT / stateful connection tracking / port filtering. This is a stateless,
  per-packet router, not a firewall or NAT gateway.

## 10. Summary

The earlier versions of this project moved Ethernet frames between two wires and hoped
IP addressing would sort itself out, that only works when both endpoints are on the same
subnet. The final version owns real IP addresses on each interface, speaks ARP as a real
gateway would, maintains a live MAC cache per interface, makes destination-subnet-based
forwarding decisions, resolves next-hop MACs on demand, and mutates each packet (TTL,
checksum, Ethernet addressing) exactly the way a genuine IP router is required to. That's
the only way two VMs on different subnets can be made to talk to each other through it.
