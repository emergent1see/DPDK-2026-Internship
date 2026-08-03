# DPDK-2026-Internship

# DPDK Networking Internship Project

This repository documents my internship project exploring DPDK (Data Plane Development
Kit), userspace networking, and kernel-bypass packet processing. It starts from first
principles (why DPDK exists at all, how it differs from normal kernel networking) and
builds up through a working packet sniffer, a custom forwarding application, and finally
a real Layer-3 router running across a three-VM lab environment.

The goal throughout has been to understand every piece from the ground up rather than
adapt example code. Each stage in this repo was written by hand, with the reasoning
behind every design decision documented alongside it.

## Why DPDK

Normal kernel networking has to move every packet through the kernel's network stack:
interrupts fire, the packet gets copied between kernel and user space, and the kernel
does routing/firewall/socket-matching work on every single packet. At high packet rates
(millions of packets per second), that overhead becomes the bottleneck, not the network
hardware itself.

DPDK works around this with kernel bypass. A NIC is unbound from its normal kernel
driver and rebound to a userspace-friendly driver (`vfio-pci` in this project, using
IOMMU protection for safe DMA). From that point on, a userspace application talks to the
NIC directly: no interrupts, no per-packet syscalls, no kernel copies. Packets are
received and transmitted in bursts, using Poll Mode Drivers (PMDs) that continuously
poll hardware descriptor rings instead of waiting on interrupts.

The core building blocks used throughout this repo:

- **EAL (Environment Abstraction Layer):** DPDK's init layer, handles hugepage setup,
  core/thread pinning, PCI device discovery, and command-line argument parsing.
- **Hugepages:** large (2MB+) memory pages used for all DPDK memory pools, reducing TLB
  pressure at high packet rates compared to standard 4KB pages.
- **`rte_mbuf`:** DPDK's packet buffer structure, allocated from hugepage-backed memory
  pools (`rte_mempool`), holding the raw Ethernet frame plus metadata.
- **Descriptor rings:** the RX/TX queues a PMD uses to hand packets to and from the NIC
  in bursts.
- **VFIO/UIO + IOMMU:** the mechanism that lets a userspace process safely DMA directly
  with the NIC, with IOMMU providing memory protection so a buggy or malicious userspace
  driver can't scribble over arbitrary physical memory.

For context, DPDK isn't the only kernel-bypass approach out there. XDP runs eBPF
programs at the earliest point in the kernel's own RX path (bypassing most of the stack
without leaving the kernel entirely), and netmap takes a similar userspace-ring approach
to DPDK with a smaller footprint. DPDK was chosen here for the depth of control it gives
over every stage of packet handling, and because that depth is exactly what makes it
useful for learning the underlying concepts properly.

## Project Structure

```
.
├── docs/
│   └── DPDK-L3-Router-Setup.md    # Full VM/environment setup journal + error log
├── capture/
│   ├── dpdk_port.h / dpdk_port.c  # Port init helpers (shared across apps)
│   └── dpdk_capture_app.c         # Custom packet sniffer: Ethernet/IPv4/UDP/TCP parsing
├── router/
│   └── dpdk_router.c              # Final Layer-3 router (two subnets, real ARP + routing)
├── test-apps/
│   ├── server.c                   # Minimal TCP server used to verify end-to-end forwarding
│   └── client.c                   # Minimal TCP client, sends "Hi", expects "Hello"
└── README.md
```

(Adjust paths above to match however the repo is actually organized, this reflects the
logical grouping of what's been built so far.)

## What's Been Built, In Order

### 1. Environment from scratch

Two full environment builds: an initial attempt on Oracle Linux that had to be
abandoned after a VirtualBox chipset change (PIIX3 to ICH9, required for IOMMU
emulation) orphaned the virtual disk, and a full rebuild on Ubuntu Server 24.04 LTS that
became the working environment. That rebuild covered IOMMU group enablement,
hugepage configuration, `vfio-pci` module loading and NIC binding, and a DPDK
build-from-source (which hit an out-of-memory kill and needed `-j2` plus a swapfile to
complete on a small VM).

Every error hit during this phase, and how it was diagnosed and fixed, is logged in
[`docs/DPDK-L3-Router-Setup.md`](docs/DPDK-L3-Router-Setup.md).

### 2. Packet capture application

A from-scratch packet sniffer (`dpdk_capture_app.c`) that pulls raw frames off a
DPDK-bound NIC and manually parses Ethernet, IPv4, UDP, and TCP headers, printing a
hex-dump and decoded summary of each packet. Writing this by hand (rather than using a
prebuilt example) is what forced a real understanding of:

- Pointer semantics when walking a raw byte buffer as a chain of casted header structs.
- Network vs. host byte order, and exactly which fields need `rte_be_to_cpu_*`
  conversions and which don't (single-byte fields never do).
- Struct initializer conventions and DPDK's return-code discipline (checking `< 0`
  consistently rather than assuming success).

Real traffic captured and explained during testing included BitTorrent Local Peer
Discovery multicast and mDNS traffic from the host machine, useful for understanding
what "normal background noise" looks like on a LAN segment before intentionally
generated test traffic is added on top (Scapy was used on the Windows host, via Npcap in
WinPcap-compatible mode, to generate controlled test packets).

### 3. Layer-2 relay (first forwarding attempt)

A minimal two-port relay that blindly retransmits whatever frame arrives on one DPDK
port out the other. This works correctly only when both endpoints believe they're on the
same L2 segment (same subnet, no gateway needed), and was the first proof that the
core RX/TX/mempool plumbing was solid before adding any routing logic on top.

### 4. Layer-3 router (current, final stage)

The current centerpiece of the project: a real router (`dpdk_router.c`) sitting between
two VMs on two different subnets. Unlike the L2 relay, this application:

- Owns an IP address on each port and acts as the default gateway for each VM's subnet.
- Answers ARP requests for its own gateway IPs.
- Learns each VM's real IP/MAC dynamically from both ARP and IP traffic.
- Performs an actual subnet-based forwarding decision, then resolves the next-hop MAC
  before transmitting (sending an ARP request and dropping the packet on a cache miss,
  exactly like a real router or Linux box does).
- Decrements TTL and recomputes the IPv4 header checksum on every forwarded packet,
  since that's mandatory router behavior that a pure bridge must not do.
- Rewrites the Ethernet source/destination MAC per hop, while leaving IP addresses and
  the TCP/UDP payload completely untouched.

Verified working for both ICMP (`ping`) and TCP (custom client/server plus `nc`) across
the two subnets. The full build/setup/debugging story, including every wrong turn, is in
[`docs/DPDK-L3-Router-Setup.md`](docs/DPDK-L3-Router-Setup.md).

## Lab Topology

```
VM2 (192.168.100.2/24)  <---->  [ DPDK middlebox ]  <---->  VM1 (192.168.200.2/24)
                          Port0                     Port1
                     gateway .1                gateway .1
```

The middlebox VM owns two NICs, each bound to `vfio-pci` and each acting as the gateway
IP for the subnet it faces. Full VM network configuration (both the same-subnet bridge
test and the final cross-subnet setup) is documented in the setup journal.

## Building

```bash
meson setup build
cd build
ninja
sudo ./<binary-name> -l 0-1 -n 2
```

Each app under `capture/`, `router/`, etc. has its own `meson.build`, see the individual
directory for exact binary names and any app-specific flags (e.g. `--disable-link-check`,
needed on VirtualBox's emulated e1000 NICs).

## What I'm Still Working Through

- Proactive ARP resolution on startup, to avoid the guaranteed first-packet drop on a
  cold cache.
- ICMP error generation (`Time Exceeded`, `Destination Unreachable`) instead of silent
  drops, to make the router spec-compliant rather than just functionally correct.
- A less naive ARP cache (currently a fixed 16-slot table per port with dumb eviction).
- Comparing this hand-rolled router's behavior against `l3fwd`, DPDK's own reference L3
  forwarding sample application, once the basics here are solid.

## Background

Alongside this internship project, I've studied artificial neural networks academically
(perceptrons, backpropagation, gradient descent) and worked on university machine
learning coursework, and independently explored a few of my own project ideas outside of
formal coursework. This repo is specifically scoped to the DPDK/networking side of that
work.
