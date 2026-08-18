# Complete Guide: DPDK Router, Jetson AGX Orin, and Laptop Connectivity, Traffic Generation and Analysis

## What this guide covers

This is the complete walkthrough of our DPDK L3 router setup, connecting a Windows laptop and a Jetson AGX Orin through a bare metal Ubuntu machine running DPDK in userspace, plus how we generate real traffic through that router and actually see and analyze what's happening. We wrote this so someone with zero prior context on our project could read it start to finish and reproduce the entire thing themselves.

## The big picture

```
Laptop 1                    Ubuntu DPDK Router                  Jetson AGX Orin
172.20.10.10/24         (kernel bypass, vfio-pci)               192.168.30.10/24
      |                                                                |
      |  port 0                                              port 1   |
      +------------------------> jetson_router <---------------------+
                                 Port 0 = 172.20.10.1
                                 Port 1 = 192.168.30.1
```

The Ubuntu machine sits physically in the middle, with two of its NICs completely detached from the Linux kernel and handed over to our own userspace application. Our application, not the kernel, is what receives packets, decides where they go, rewrites their headers, and sends them back out. This is the entire point of DPDK, we get to see every part of the process that a normal router or the Linux kernel would otherwise hide from us.

## Part 1, the DPDK machine itself

### Hardware and prerequisites

Our Ubuntu box is a bare metal, dual socket Xeon workstation with real hardware IOMMU support (Intel VT-d). We confirmed this early on with:

```bash
dmesg | grep -e DMAR -e IOMMU
```

We found eight separate IOMMU units and two NUMA nodes, confirming a genuine dual socket server class board. We also checked which NUMA node our two DPDK facing NICs sit on:

```bash
cat /sys/bus/pci/devices/0000:86:00.0/numa_node
cat /sys/bus/pci/devices/0000:d8:00.0/numa_node
```

Both came back as node 1, meaning we could pin all our DPDK cores and memory to that same node and avoid any cross socket memory penalty. We found the local cores for that node with:

```bash
cat /sys/devices/system/node/node1/cpulist
```

which gave us cores 10 through 19 and 30 through 39. We use cores 10, 11, and 12 throughout this guide.

### Installing DPDK

```bash
sudo apt update
sudo apt install -y build-essential meson ninja-build python3-pyelftools python3-pip libnuma-dev pkg-config git linux-headers-$(uname -r) net-tools pciutils

git clone https://github.com/DPDK/dpdk.git
cd dpdk
meson setup build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

### Reserving huge pages

We reserve huge pages specifically on NUMA node 1, since that's where our NICs and cores live:

```bash
echo 1024 | sudo tee /sys/devices/system/node/node1/hugepages/hugepages-2048kB/nr_hugepages
grep -i huge /proc/meminfo
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

We confirm the reservation actually landed by checking `HugePages_Total` and `HugePages_Free` in `/proc/meminfo`, and by reading the node specific file directly rather than assuming a runtime request always succeeds in full.

### Binding the NICs to DPDK

We identified our two physical NICs, `enp134s0f0` at PCI address `0000:86:00.0`, and `enp216s0f0` at PCI address `0000:d8:00.0`. Both are real 10 Gigabit Ethernet cards, model X540-AT2. We confirmed they sit in separate IOMMU groups before binding, so we could bind each independently without risk of pulling in an unrelated device:

```bash
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:86:00.0
sudo dpdk-devbind.py --bind=vfio-pci 0000:d8:00.0
sudo dpdk-devbind.py --status
```

We always leave our management NIC, the one carrying our own SSH session, completely untouched. On our box that's `enp216s0f1`, confirmed separately with `ip a` and `ip route get 8.8.8.8` before ever touching anything DPDK related.

## Part 2, the IP addressing scheme, and a real lesson we learned

We designed our addressing so the two endpoints, the laptop and the Jetson, sit on genuinely different subnets, forcing our router to do real Layer 3 work, actual IP header inspection and forwarding decisions, rather than just blindly bridging frames at Layer 2.

We originally used `192.168.10.0/24` for the laptop's leg, but discovered it collided with a real switch already present on our university network, since some of our machines also touch that broader network through other interfaces. We moved to `172.20.10.0/24` instead, a range far less commonly used by default configurations, and never had the problem again. This is worth checking for on any network that isn't fully isolated before picking private ranges of your own.

Our final scheme:

```
Laptop 1:                 172.20.10.10/24, gateway 172.20.10.1
Router leg facing laptop: 172.20.10.1/24
Router leg facing Jetson: 192.168.30.1/24
Jetson:                   192.168.30.10/24, gateway 192.168.30.1
```

## Part 3, the router application itself

We wrote `jetson_router.c`, a clean, minimal two leg router. It deliberately does not include any internet forwarding or NAT logic, since that wasn't part of this goal, just genuine routing between our two known subnets.

What it actually does, on every packet:

It answers ARP requests for its own two interface IPs directly, and learns every sender's IP to MAC mapping it sees, whether from an ARP packet or plain IP traffic.

It answers ICMP echo requests, pings, addressed to its own interface IPs, building a real reply in place rather than trying to re-forward the packet to itself.

It recognizes subnet broadcast addresses and drops them immediately, rather than endlessly retrying an ARP request that can never be answered.

For everything else, it looks up the destination IP against its own small routing table, decides which port to send it out of, decrements the IP time to live, recalculates the header checksum, resolves the next hop MAC address through its own ARP cache, rewrites the Ethernet header, and transmits, exactly the sequence of steps a real router performs on every single hop.

We also batch our transmissions. Early on our code called `rte_eth_tx_burst` once per single packet, which throws away most of the benefit of DPDK's burst oriented design. We restructured it so packets destined for the same output port during one poll cycle get collected together and flushed as one real burst call instead of many tiny ones.

### Building and running it

```bash
cd ~/dpdk/code-files
gcc -O3 jetson_router.c -o jetson_router $(pkg-config --cflags --libs libdpdk)
sudo ./jetson_router -l 10,11,12 -n 4 --socket-mem=0,512 -a 0000:86:00.0 -a 0000:d8:00.0
```

One important lesson we learned the hard way, worth stating plainly. DPDK assigns port numbers by ascending PCI bus address, not by the order flags are listed on the command line. We proved this to ourselves by swapping the order of our two `-a` flags and watching the exact same port mapping come out both times. Because of this, we never assume which physical device ended up as which port number, we confirm it every time from the live startup log, specifically the `ARP Learned` lines that print as real traffic starts flowing, they show exactly which real device address is showing up on which port.

## Part 4, connecting Laptop 1

On Laptop 1, a Windows machine, we set the Ethernet adapter facing the router to a static address through its IPv4 properties:

```
IP address:      172.20.10.10
Subnet mask:      255.255.255.0
Default gateway:  172.20.10.1
```

Since our router has no default route logic in this clean version, every subnet crossing needs an explicit route. We added the specific route to reach the Jetson's subnet, from an elevated Command Prompt:

```
route -p add 192.168.30.0 mask 255.255.255.0 172.20.10.1
```

We confirmed both with `ipconfig` and `route print`.

One thing worth watching for, if Wi-Fi is also active on the laptop, Windows installs its own separate default route through it, and depending on which route has the lower metric, traffic meant for our router can get silently sent out Wi-Fi instead and simply disappear. The specific route we added above always wins over a general default route regardless of metric, since a more specific subnet match is always preferred, but it's worth knowing this exists as a source of confusing failures if Wi-Fi is toggled on and off during testing.

## Part 5, connecting the Jetson AGX Orin

Our Jetson is a completely headless box, no display attached at all. We reached it over SSH using a Wi-Fi connection it already had to our university network, on an interface called `wlP1p1s0`, purely as our way in, we never touched or reconfigured that connection itself.

```bash
ssh agx-1@10.1.81.191
```

Once logged in, we identified the actual wired Ethernet interface facing our DPDK router, separate from Wi-Fi and from a couple of virtual bridges NVIDIA's own tooling and Docker had created:

```bash
ifconfig
```

`docker0` and `l4tbr0` are virtual bridges, unrelated to our physical wiring. The interface that mattered was `eno1`, confirmed by the `RUNNING` flag and a real transmitted packet count, showing it was genuinely linked up to our Ubuntu box.

We assigned it the static address on our scheme:

```bash
sudo ip addr add 192.168.30.10/24 dev eno1
sudo ip link set eno1 up
sudo ip route add 172.20.10.0/24 via 192.168.30.1
```

We hit one subtle bug here worth mentioning. After a reboot, we re-added this address but accidentally left off the subnet size, ending up with `192.168.30.10/32` instead of `/24`. A `/32` means a single host address with no network at all, so the kernel correctly refused to accept `192.168.30.1` as a valid gateway, since as far as it could tell nothing else existed in that range. We caught this by checking `ip a show eno1` carefully and fixed it by deleting and re-adding the address with the correct `/24`.

## Part 6, confirming connectivity before generating any real load

We always test in this order, each step proves one specific thing, so a failure tells us exactly where to look rather than guessing across the whole chain.

Self ping first, from Laptop 1:
```
ping 172.20.10.1
```
and from the Jetson:
```bash
ping 192.168.30.1
```
Both of these are the router answering for its own interface directly, proving the physical link and ARP resolution work.

Then peer to peer, from the Jetson toward Laptop 1:
```bash
ping 172.20.10.10
```
Watching the router console for a line confirming the packet was recognized and forwarded across.

## Part 7, generating traffic, and where to actually see it

Everything happens visibly in one place, the terminal running `jetson_router`. It prints one line per forwarded packet during light testing, and once we started pushing real sustained load, we added a periodic throughput summary instead, printed once a second, showing packets per second and megabits per second per port, along with hardware level receive miss, receive error, and transmit error counts, so we could tell a genuine hardware problem apart from simple link saturation.

### Method one, our own simple generator

We wrote `jetson_traffic_gen.py`, a small Python script using plain sockets, no extra libraries needed, that sends UDP packets at a controllable rate with a sequence number in each payload, useful for correlating exactly what was sent against what the router logged and what actually arrived.

```bash
python3 jetson_traffic_gen.py --dest 172.20.10.10 --port 9000 --rate 2
```

### Method two, iperf3 between the Jetson and Laptop 1

This is what let us actually measure real sustained throughput rather than just proving connectivity. We installed iperf3 on the Jetson with `sudo apt install -y iperf3`, and downloaded the Windows build directly onto Laptop 1.

Server, on Laptop 1:
```
iperf3.exe -s -i 1
```

Client, on the Jetson:
```bash
iperf3 -c 172.20.10.10 -u -b 900M -t 20 -i 1
```

### Method three, generating traffic directly from the DPDK machine itself, using a TAP device

We wanted to push traffic through our router without relying on either endpoint's own NIC speed. Since `iperf3` is an ordinary program that can only send through a real kernel network interface, and our DPDK bound ports have no kernel presence at all, we used DPDK's own TAP virtual device feature to bridge the two worlds.

We loaded the kernel module TAP needs:
```bash
sudo modprobe tun
```

We changed our router's launch command to allowlist only the real physical port facing the Jetson, plus a new virtual TAP device standing in for the laptop's leg:
```bash
sudo ./jetson_router -l 10,11,12 -n 4 --socket-mem=0,512 -a 0000:d8:00.0 --vdev=net_tap0,iface=dtap0 --
```

This created a real, normal, kernel visible interface called `dtap0` on the Ubuntu box itself. We configured it exactly like any other leg in our scheme:
```bash
sudo ip addr add 192.168.40.2/24 dev dtap0
sudo ip link set dtap0 up
sudo ip route add 192.168.30.0/24 via 192.168.40.1 dev dtap0
```

With this in place, we could run `iperf3` directly on the Ubuntu machine itself, an ordinary process using an ordinary socket, and its traffic would leave through `dtap0`, land inside our DPDK application exactly like traffic from any other leg, get routed through our real forwarding logic, and go out the genuine physical port toward the Jetson, letting us test this specific link's true capacity, cleanly separated from any laptop hardware limitation.

```bash
iperf3 -c 192.168.30.10 -u -b 900M -t 20 -i 1
```

run directly on the Ubuntu box, with `iperf3 -s` running on the Jetson as the listener.

## Part 8, reading the results honestly

During our high rate tests, we saw the Jetson push over 1.4 Gbps into the router, received cleanly with zero hardware receive errors, but the router's leg facing Laptop 1 consistently topped out at almost exactly 984 Mbps, no matter how much harder we pushed. That precise, repeated number is the practical achievable throughput of a 1 Gigabit Ethernet link once real Ethernet, IP, and UDP overhead is accounted for. It told us plainly that Laptop 1's onboard NIC hardware, not our router's software, was the actual limiting factor on that leg. We confirmed this by throttling the Jetson below that ceiling and watching loss disappear almost entirely.

We also found that loss can appear well under a hard ceiling too, caused by iperf3's own bursty sending pattern briefly overflowing a receive buffer even at a moderate average rate. Increasing the receiving side's socket buffer with `iperf3.exe -s -w 4M` reduced this significantly, though it introduced a small amount of packet reordering as a real, expected tradeoff, more packets genuinely in flight simultaneously means more opportunity for their arrival order to shift slightly.

## Part 9, actually seeing the packets themselves, not just counting them

Throughput numbers tell us how much moved, not what was inside it. Since our DPDK bound ports have no kernel presence, ordinary tools like `tcpdump` cannot see anything on them at all. DPDK has its own proper mechanism for this, the `dpdk-pdump` tool, which attaches to our already running application through EAL's own multi process support and pulls a genuine copy of the traffic into a standard pcap file.

We enabled this with a single line added near the start of our application, `rte_pdump_init()`. From a second terminal, while the router kept running, we captured live:

```bash
sudo dpdk-pdump -- --pdump 'port=0,queue=*,rx-dev=/tmp/capture.pcap'
```

and pulled the resulting file back to our own laptop with `scp` to open directly in Wireshark, giving us real Ethernet, IP, and transport layer headers, and real payload bytes, of traffic that existed entirely in DPDK's own userspace memory the whole time.

## Summary checklist

Confirm real hardware IOMMU support and NUMA node placement before doing anything else, and reserve huge pages specifically on the correct node.

Bind only the intended physical NICs to `vfio-pci`, always leaving the management interface untouched, and confirm this fresh every session since bindings do not survive a reboot on their own.

Pick a private IP range confirmed not to collide with any real infrastructure reachable from any of the machines involved.

Build and run the router, and always confirm real port to device mapping from the live ARP learned log, never assume it from command line flag order.

Configure each endpoint with a static IP and explicit routes to the other subnet, since our router has no default route logic, every subnet crossing must be told about directly.

Prove connectivity in layers, self ping, then peer to peer ping, before generating any real load.

Use the simple Python generator for easily correlated low rate tests, iperf3 between real endpoints for genuine throughput measurement, and a DPDK TAP device when we want to generate traffic directly from the DPDK machine itself, bypassing endpoint hardware limitations entirely.

Read throughput ceilings carefully, a suspiciously precise, repeated number across many samples is usually a real physical link limit, not a software bug, and can be confirmed by throttling the source below it and watching loss disappear.

Use `dpdk-pdump` and Wireshark whenever we need to see actual packet contents, since no ordinary kernel based capture tool can see anything on a `vfio-pci` bound interface at all.
