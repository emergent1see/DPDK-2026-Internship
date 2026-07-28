# Setting Up DPDK in a VM

This is a record of how I set up a complete DPDK development environment inside a VirtualBox virtual machine, wrote a custom packet capture application, and got real traffic flowing between my Windows host and the VM. I ran into a good number of real problems along the way, and I'm documenting them here exactly as I hit them, along with how I fixed each one, in case it saves someone else the same hours I spent.

## What I was working with

- **Host machine:** Windows, using VirtualBox as my hypervisor
- **Guest OS:** Ubuntu Server 24.04 LTS
- **Goal:** get DPDK built and running, bind a NIC to it, write my own capture application, and prove real traffic flowing end to end

---

## Table of Contents

1. [Picking the OS and hypervisor](#picking-the-os-and-hypervisor)
2. [Creating the VM](#creating-the-vm)
3. [Enabling IOMMU and VFIO](#enabling-iommu-and-vfio)
4. [Installing dependencies](#installing-dependencies)
5. [Setting up hugepages](#setting-up-hugepages)
6. [Building DPDK](#building-dpdk)
7. [Binding a NIC to DPDK](#binding-a-nic-to-dpdk)
8. [Verifying with testpmd](#verifying-with-testpmd)
9. [Writing my own capture application](#writing-my-own-capture-application)
10. [Getting code onto the VM](#getting-code-onto-the-vm)
11. [Generating real traffic from Windows](#generating-real-traffic-from-windows)
12. [Reading the captured packets](#reading-the-captured-packets)
13. [What I actually learned about DPDK](#what-i-actually-learned-about-dpdk)

---

## Picking the OS and hypervisor

I went with **Ubuntu Server 24.04 LTS** for the guest. DPDK's own documentation is written with Ubuntu/Debian in mind, the package manager has everything I needed, and any time I got stuck there was a huge amount of existing discussion to search through.

For the hypervisor, since I'm on Windows, I used **VirtualBox**. I know KVM performs better and is the more "real" choice for production style testing, but I wasn't about to dual boot Linux just for this, so VirtualBox it was.

## Creating the VM

I gave the VM:
- 8192 MB RAM
- 4 vCPUs
- 20 GB disk

One setting I changed early, because I already knew I'd need it later for VFIO: I went into **Settings → System → Motherboard → Chipset** and switched it from PIIX3 to **ICH9**. This gives the VM PCIe emulation, which VFIO passthrough depends on.

I also added a **second network adapter**, set to **Host-only Adapter**, specifically so I'd have an interface I could hand over to DPDK without losing my only connection to the internet and SSH.

I installed Ubuntu Server normally, made sure to check **OpenSSH server** during setup so I could work over a real terminal instead of the tiny VirtualBox console window, and rebooted once it finished.

## Enabling IOMMU and VFIO

This is where I hit my first real snag.

### The problem: 0 IOMMU groups

After setting `intel_iommu=on iommu=pt` in `/etc/default/grub` and rebooting, I ran:

```bash
find /sys/kernel/iommu_groups/ -type l | wc -l
```

It came back `0`. No IOMMU groups at all, meaning VFIO wasn't going to work no matter what I did in the guest.

**Why:** switching the chipset to ICH9 only turns on PCIe emulation. It does *not* automatically turn on IOMMU emulation. In VirtualBox 7.0+, IOMMU type is a completely separate setting, and it isn't even exposed in the GUI. I only found this after digging.

**The fix**, run on the Windows host with the VM fully powered off:

```powershell
VBoxManage modifyvm "dpdk-server" --iommu=intel
```

After that, I booted the VM back up and reran the check. This time it returned a real number, confirming the IOMMU was actually active.

## Installing dependencies

Straightforward, no surprises here:

```bash
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y git build-essential meson ninja-build \
  python3-pyelftools python3-pip libnuma-dev pkg-config \
  linux-headers-$(uname -r) libssl-dev zlib1g-dev libpcap-dev pciutils kmod
```

I checked my meson version out of caution:

```bash
meson --version
# 1.3.2
```

DPDK only needs 0.57 or newer, so `1.3.2` was already well above the minimum. But I wanted to be on the latest release anyway, so I went ahead and upgraded it.

### Problem: pip refused to touch the system Python

```bash
pip install meson
```

```
error: externally-managed-environment
This environment is externally managed
```

Ubuntu 24.04 protects the system Python from `pip install` by default, under PEP 668, specifically so it doesn't clash with packages `apt` is also managing.

**The fix:** I bypassed that protection deliberately, since this is my own dev VM and I know exactly what I'm overriding:

```bash
sudo python3 -m pip install --break-system-packages --force-reinstall meson==1.11.1
```

`--break-system-packages` is what actually gets past the PEP 668 block. `--force-reinstall` made sure it installed cleanly even with the `apt`-provided version already sitting there. If a version was already present and this alone didn't take, adding `--ignore-installed` forces pip to install fresh over top of it regardless:

```bash
sudo python3 -m pip install --break-system-packages --ignore-installed meson==1.11.1
```

Confirmed it took:

```bash
meson --version
# 1.11.1
```

## Setting up hugepages

```bash
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
echo "vm.nr_hugepages=1024" | sudo tee -a /etc/sysctl.conf
sudo mkdir -p /mnt/huge
echo "nodev /mnt/huge hugetlbfs defaults 0 0" | sudo tee -a /etc/fstab
sudo mount -a
```

Minor hiccup: after editing `/etc/fstab`, `sudo mount -a` threw a systemd warning about a unit file changing on disk. Not an actual error, just needed:

```bash
sudo systemctl daemon-reload
sudo mount -a
```

Confirmed with `grep Huge /proc/meminfo` that `HugePages_Total` matched what I reserved.

## Building DPDK

```bash
cd ~
git clone https://github.com/DPDK/dpdk.git
cd dpdk
meson setup build
ninja -C build
```

### The problem: the build got killed mid compile

Partway through, I got:

```
cc: fatal error: Killed signal terminated program cc1
compilation terminated.
ninja: build stopped: subcommand failed.
```

This was the Linux OOM killer. `ninja` defaults to compiling with as many parallel jobs as I have cores, and some of the DPDK driver files (the `cnxk` ones especially) are large enough that running four compiles at once exhausted my VM's RAM.

**The fix:**

```bash
sudo ninja -C build -j2 install
```

Limiting to 2 parallel jobs got me through without crashing. For extra safety on future builds, I also added swap:

```bash
sudo fallocate -l 4G /swapfile
sudo chmod 600 /swapfile
sudo mkswap /swapfile
sudo swapon /swapfile
echo '/swapfile none swap sw 0 0' | sudo tee -a /etc/fstab
```

Once it finished:

```bash
sudo ninja -C build install
sudo ldconfig
```

## Binding a NIC to DPDK

This step took a few tries to get right.

### Problem 1: "interface is active, not modifying"

My first attempt:

```bash
sudo dpdk-devbind.py --bind=vfio-pci 0000:00:08.0
```

```
Warning: routing table indicates that interface 0000:00:08.0 is active. Not modifying
```

`dpdk-devbind.py` refuses to touch an interface that still has an active route/IP assigned, as a safety guard. I had to bring the link down first:

```bash
sudo ip link set enp0s8 down
sudo dpdk-devbind.py --bind=vfio-pci 0000:00:08.0
```

That worked.

### Problem 2: I accidentally bound my NAT adapter too

At one point I ran `--status` and realized **all three** of my NICs, including `00:03.0`, my NAT adapter, had ended up bound to `vfio-pci`. That's the one keeping my SSH session and internet alive. I had to give it back:

```bash
sudo dpdk-devbind.py --bind=e1000 0000:00:03.0
```

and manually restore its network connection afterward, since it didn't always come back with an IP automatically:

```bash
sudo ip addr add 192.168.56.102/24 dev enp0s8
sudo ip link set enp0s8 up
```

### Getting the end state right

What I actually wanted, and eventually got, was:

- `0000:00:03.0` (NAT) → kernel driver, `e1000`, stays up for SSH
- `0000:00:08.0` and `0000:00:09.0` → `vfio-pci`, available for DPDK

```bash
dpdk-devbind.py --status
```

confirmed the split was correct.

## Verifying with testpmd

```bash
sudo dpdk-testpmd -l 0-3 -n 4 -- -i
```

Inside the prompt:

```
testpmd> show port info all
```

### Problem: link status showing down

Even with everything else correctly configured, my port showed:

```
Link status: down
Link speed: None
```

I checked "Cable Connected" in VirtualBox's adapter settings (it was already ticked), tried `port start 0`, tried restarting the port. Still down.

**What was actually going on:** my Adapter 3 was attached to an Internal Network with no other VM on the other end, so there was genuinely nothing to link to. It wasn't a misconfiguration, it was an accurate report of an empty virtual wire.

**The workaround**, for testing purposes:

```bash
sudo dpdk-testpmd -l 0-3 -n 4 -- -i --disable-link-check
```

Worth being clear about what this actually does: it only removes testpmd's software gate that waits for a reported link-up before starting forwarding threads. It does not create connectivity out of nothing. On a port with a real peer (like my host-only adapter, which has my Windows host on the other end), this let real traffic through. On the fully isolated Internal Network port, it just meant the app didn't crash waiting for a link that would never come.

## Writing my own capture application

I wrote a custom C application based on DPDK's `skeleton` example, one that parses Ethernet, IPv4, UDP/TCP headers and hex dumps the payload instead of just counting packets.

### Problem: assignment instead of comparison

While retyping code by hand from my notes into `nano` on the VM, I introduced:

```c
if (mbuf_pool = NULL)   // bug
```

instead of:

```c
if (mbuf_pool == NULL)   // correct
```

Classic C typo. The single `=` overwrote my pool with `NULL` and, because the assignment itself evaluates to false, skipped the error check entirely, letting a broken null pool flow silently into the rest of the program.

### Problem: missing while clause on a do-while loop

Another retyping casualty. I'd written a link-status retry loop as a `do { ... }` block but dropped the closing `while (...)` when copying it over:

```c
// broken
do {
    rte_delay_ms(100);
    rte_eth_link_get_nowait(port, &link);
    if (link.link_status == RTE_ETH_LINK_UP)
        break;
}
```

```c
// fixed
do {
    rte_delay_ms(100);
    rte_eth_link_get_nowait(port, &link);
    if (link.link_status == RTE_ETH_LINK_UP)
        break;
} while (--retries > 0);
```

`gcc` correctly refused to compile it: `expected 'while' before 'int'`.

### Problem: forgot sudo

Ran into:

```
EAL: Couldn't get fd on hugepage file
EAL: FATAL: rte_service_init() failed
```

Turned out I'd just left `sudo` off that particular run. Without root, DPDK can't open the hugepage-backed memory it needs. Re-running with `sudo ./dpdk_capture_app -l 0-1 -n 4` fixed it immediately.

### Problem: crash on first packet poll

The app built fine and started, but crashed instantly with:

```
lcore 0 called rx_pkt_burst for not ready port 0
```

This was a genuine race condition in my own code, not a typo. I was calling `rte_eth_rx_burst()` immediately after `rte_eth_dev_start()` returned, but on this particular emulated NIC, the RX queue doesn't finish transitioning to "started" state instantly. My first poll could land in that gap.

**Fix:** add a short settle delay and link check right after starting the device, before entering the polling loop:

```c
struct rte_eth_link link;
int retries = 20;
do {
    rte_delay_ms(100);
    rte_eth_link_get_nowait(port, &link);
    if (link.link_status == RTE_ETH_LINK_UP)
        break;
} while (--retries > 0);

printf("Port %u link status: %s (continuing regardless)\n",
       port, link.link_status == RTE_ETH_LINK_UP ? "UP" : "DOWN");
fflush(stdout);
```

I also added `fflush(stdout)` after my key print statements, since I learned that if the process aborts on a crash, buffered output can get lost before it ever reaches the screen, which cost me some confused debugging earlier.

## Getting code onto the VM

### Problem: clipboard wasn't bidirectional

Copy-pasting code from my notes into the VirtualBox console window kept failing silently or partially, which is exactly how both of the typo bugs above happened. Shared Clipboard wasn't set to bidirectional, and Guest Additions wasn't fully installed either.

**What actually solved it long term:** I stopped relying on clipboard entirely and used **SCP** instead, copying files straight from Windows to the VM over the SSH connection I already had working:

```powershell
scp C:\Users\USER\Downloads\dpdk_capture_app.c atta@192.168.56.102:~/dpdk-apps/capture-app/
```

This transfers the file byte for byte, no retyping, no clipboard involved. Every code fix after that point went over cleanly on the first try.

## Generating real traffic from Windows

I wanted to actually send packets from my host and watch them get received and decoded on the VM, not just observe background noise.

### Problem: plain ping never arrived

Pinging the VM's DPDK-bound IP from Windows just returned `Request timed out` every time. The reason: with the interface bound to `vfio-pci`, there's no kernel network stack on the VM side to answer ARP requests. Windows would try to resolve the destination MAC first, get no answer, and never even send the actual ICMP packet.

### The fix: bypass ARP with Scapy

Instead of relying on ping, I used **Scapy** on Windows to build the Ethernet frame myself, with the destination MAC already filled in, skipping ARP resolution completely.

Setting Scapy up had its own hiccup:

```
WARNING: No libpcap provider available ! pcap won't be used
ImportError: cannot import name 'get_windows_if_list' from 'scapy.all'
```

Scapy on Windows needs **Npcap** installed to do anything with raw packets at all. I hadn't installed it. After downloading Npcap and specifically checking **"Install Npcap in WinPcap API-compatible Mode"** during setup, then restarting my terminal, both problems went away.

With that sorted, sending a real crafted packet looked like this:

```python
from scapy.all import Ether, IP, UDP, sendp

sendp(
    Ether(dst="08:00:27:74:c8:3b") /
    IP(dst="192.168.56.102") /
    UDP(dport=9999) /
    b"Hello from Windows host - real DPDK test payload!",
    iface="Ethernet 3"
)
```

## Reading the captured packets

Once my capture app was stable, I ran it in verbose mode and started seeing real traffic land, including some I hadn't intentionally generated.

**BitTorrent Local Peer Discovery**, sent to a multicast address (`239.192.152.143:6771`), with a payload I could read directly as ASCII in my hex dump:

```
BT-SEARCH * HTTP/1.1
Host: 239.192.152.143:6771
Port: 6881
Infohash: 62a4d9e1...
```

Turned out a BitTorrent client running on my Windows host was periodically broadcasting these discovery announcements over every active interface, including the host-only adapter, which explained why my VM was seeing them at all.

**mDNS traffic**, to the well known `224.0.0.251` multicast address, revealing my own hostname:

```
SYK-Laptop._dosvc._tcp.local
```

Both were completely real, legitimate background traffic from my own machine, not errors and not anything I'd caused directly. Being able to explain exactly where each one came from was actually a good moment, since it meant my parsing code was correctly pulling apart real world protocol data, not just counting bytes.

## What I actually learned about DPDK

Somewhere in the middle of debugging binds and typo bugs, I asked myself what the point of any of this was, since I wasn't seeing anything I couldn't already see with Wireshark.

The answer that actually stuck: DPDK isn't about seeing data you couldn't otherwise see. It's about **speed**. The normal kernel path (interrupt per packet, generic protocol stack, memory copies between kernel and user space) becomes the actual bottleneck once you're dealing with millions of packets a second, which is completely ordinary for routers, firewalls, and telecom infrastructure. DPDK strips that overhead out by letting an application own the NIC directly, poll instead of interrupt, and avoid unnecessary copies.

The "intelligence" of a real router, its routing table, its BGP/OSPF protocol handling, doesn't disappear when DPDK is involved. It just moves into the application itself, typically as a fast lookup structure like `rte_lpm`, fed by a separate, ordinary control-plane process that still runs on the regular kernel network stack, since that part only needs to update a few times a second, not millions of times.

My VM setup proved the actual architecture, correctly: EAL init, hugepage backed memory, VFIO device ownership, real header parsing. What it can't demonstrate is the raw performance number that's the entire reason DPDK exists, since that needs real physical NIC hardware pushing real production-scale traffic, not emulated e1000 devices inside a VM.

---

## Repository structure

```
.
├── README.md                  # this file
├── dpdk_capture_app.c         # my custom packet capture application
├── send_packet.py             # Scapy script used from the Windows host
└── docs/
    └── setup-notes.md         # raw command reference, no narrative
```
