# Jetson Traffic Generation and DPDK Packet Analysis Guide

## What this covers

This document walks through everything we did to turn a Jetson AGX Orin into a traffic generator, route that traffic through our DPDK L3 router in userspace, measure real throughput with iperf3, and capture and analyze the actual packets with Wireshark. We wrote this so someone with no prior context on our setup could follow it start to finish and reproduce the whole thing on their own hardware.

## Architecture

```
Jetson AGX Orin                  Ubuntu DPDK Router                Laptop 1
192.168.30.10/24                 (kernel bypass, vfio-pci)         172.20.10.10/24
      |                                                                   |
      |  Ethernet, port 1                                  Ethernet, port 0  |
      +---------------------------> jetson_router <---------------------+
                                    Port 1 = 192.168.30.1
                                    Port 0 = 172.20.10.1
```

The Jetson generates real packets addressed to Laptop 1. They physically arrive on the Ubuntu box's port 1. Our DPDK application picks them straight off the NIC in userspace, with zero kernel involvement on that interface, decides they belong to Laptop 1's subnet, decrements the IP TTL, rewrites the Ethernet header, and transmits out port 0 toward Laptop 1. Laptop 1 receives it as a completely normal packet with no idea any of this happened in the middle.

We chose this two subnet design on purpose, rather than putting Jetson and Laptop 1 on the same subnet, because it forces the router to do genuine Layer 3 routing, actual IP header inspection and forwarding decisions, not just blind Layer 2 bridging.

## A note on our specific IP scheme

We originally used `192.168.10.0/24` for the Laptop 1 leg, but discovered partway through that this address range collided with a real switch already present on our university network. Since some of our machines also touch that broader network through other interfaces, we moved the Laptop 1 leg to `172.20.10.0/24` instead, a range far less commonly used by default configurations. We kept the Jetson leg at `192.168.30.0/24` since it never showed any collision. We mention this because it's a real trap worth checking for on any network that isn't fully isolated, before picking your own private ranges, it's worth confirming they don't already exist somewhere on infrastructure your machines can also reach.

## Step 1, physically connecting the Jetson

We repurposed the Ethernet cable and port that previously went to a second laptop. We unplugged that cable from the laptop and plugged it directly into the Jetson's onboard wired Ethernet port. On the Ubuntu box side, nothing needed to change, the physical NIC, PCI address `0000:d8:00.0`, was already bound to `vfio-pci` from earlier work and stayed that way.

## Step 2, getting a terminal on the Jetson

The Jetson AGX Orin dev kit has no display attached in our setup, it's a boxed headless device. We found it already had a working Wi-Fi connection to our university network on an interface called `wlP1p1s0`, with an address handed out by DHCP. We used that existing connection purely as our way in over SSH, we never touched or reconfigured it.

From our own laptop, on the same network as the Jetson's Wi-Fi address, we connected with:

```bash
ssh agx-1@10.1.81.191
```

using the actual username and IP that had already been assigned to this specific Jetson. Once logged in, everything else happened directly on the Jetson's own terminal, exactly like working on any other Ubuntu machine.

## Step 3, identifying the correct wired interface

Wi-Fi and the physical Ethernet port are two completely different interfaces, and we needed to configure the wired one specifically, the one now carrying the cable to our DPDK router, not the Wi-Fi one we were using to stay connected.

We ran:

```bash
ifconfig
```

and looked through the list. We found several interfaces, including `docker0` and `l4tbr0`, which are virtual bridges created by Docker and NVIDIA's own tooling, unrelated to our physical wiring, and a couple of USB gadget interfaces, also not relevant. The one that mattered was `eno1`, which showed `RUNNING` and had already transmitted real packets, confirming it was genuinely linked up to the Ubuntu box.

## Step 4, assigning the Jetson's static IP

With `eno1` identified, we assigned it an address on the Jetson's leg of our scheme:

```bash
sudo ip addr add 192.168.30.10/24 dev eno1
sudo ip link set eno1 up
```

We confirmed it landed correctly:

```bash
ip addr show eno1
```

and added a route so the Jetson knows how to reach Laptop 1's subnet through the router, since our router has no default route logic at all in this clean version, every subnet crossing needs its own explicit route on each endpoint:

```bash
sudo ip route add 172.20.10.0/24 via 192.168.30.1
```

## Step 5, configuring Laptop 1

On Laptop 1, a Windows machine, we set the Ethernet adapter facing the router to a static address:

```
IP address:      172.20.10.10
Subnet mask:      255.255.255.0
Default gateway:  172.20.10.1
```

We also added the mirror route, so Laptop 1 knows how to reach the Jetson's subnet, from an elevated Command Prompt:

```
route -p add 192.168.30.0 mask 255.255.255.0 172.20.10.1
```

We confirmed both the address and the route with `ipconfig` and `route print`.

## Step 6, the DPDK router itself

We wrote a clean, minimal two leg router in C, `jetson_router.c`, deliberately without any internet forwarding or NAT logic, since that wasn't the goal here, just real routing between these two subnets. It handles ARP requests and replies, answers ICMP pings addressed to its own interface IPs directly, drops broadcast destinations cleanly instead of endlessly retrying unanswerable ARP requests, and forwards everything else based on a real routing table lookup, with TTL decrement and IP checksum recalculation on every hop, exactly like a real router does.

We built and ran it on the Ubuntu box:

```bash
cd ~/dpdk/code-files
gcc -O3 jetson_router.c -o jetson_router $(pkg-config --cflags --libs libdpdk)
sudo ./jetson_router -l 10,11,12 -n 4 --socket-mem=0,512 -a 0000:86:00.0 -a 0000:d8:00.0
```

One important lesson from earlier in this project, worth repeating here. DPDK assigns port numbers by ascending PCI bus address, not by the order you list `-a` flags on the command line. We learned this the hard way after swapping the flag order and seeing the exact same port mapping both times. Because of this, we always confirm the real mapping from the live startup log rather than assuming, watching the `ARP Learned` lines that print as soon as either machine sends any traffic, they show exactly which real device ended up on which port number.

## Step 7, confirming basic connectivity before generating real load

We tested the simplest possible case first, from Laptop 1:

```
ping 172.20.10.1
```

and from the Jetson:

```bash
ping 192.168.30.1
```

Both of these are the router answering for its own interface IP directly, proving the physical link and ARP resolution work before testing anything that actually needs forwarding.

Then we tested the real peer to peer path, from the Jetson toward Laptop 1:

```bash
ping 172.20.10.10
```

and watched the router console for a line confirming the packet was recognized and forwarded across.

## Step 8, generating traffic, two methods

### Simple controllable generator

We wrote a small Python script, `jetson_traffic_gen.py`, that sends UDP packets at a controllable rate with a sequence number in each payload, useful for correlating exactly what was sent against what the router logged and what actually arrived at Laptop 1. No extra libraries needed, plain Python sockets, guaranteed to work on the Jetson's stock Python install.

We copied it onto the Jetson from our own laptop:

```
scp jetson_traffic_gen.py agx-1@10.1.81.191:~/
```

and ran it:

```bash
python3 jetson_traffic_gen.py --dest 172.20.10.10 --port 9000 --rate 2
```

with a simple listener running on Laptop 1 to confirm actual receipt, either a small Python one liner binding to that UDP port, or `nc -ul 9000` if Netcat was available.

### iperf3, for real sustained throughput testing

This is where we moved from a simple proof of concept to actually measuring what the router can handle under real load.

We installed iperf3 on both ends. On the Jetson:

```bash
sudo apt install -y iperf3
```

On Laptop 1, we downloaded the Windows build directly.

We started the server on Laptop 1:

```
iperf3.exe -s -i 1
```

and ran the client from the Jetson, first over plain TCP:

```bash
iperf3 -c 172.20.10.10 -t 20 -i 1
```

and then over UDP at a controlled, deliberately high rate to really push the pipeline:

```bash
iperf3 -c 172.20.10.10 -u -b 2G -t 20 -i 1
```

## Step 9, reading and understanding the results

Our first high rate UDP test showed something worth explaining properly rather than just treating as a bug. The Jetson pushed roughly 1.44 Gbps into the router. The router's console, which we had instrumented to print per second throughput statistics per port, showed port 1, facing the Jetson, receiving that full rate cleanly, no receive errors, no ring misses. But port 0, facing Laptop 1, consistently topped out at almost exactly 984 Mbps, sample after sample, remarkably stable. Meanwhile iperf3's own server output on Laptop 1 showed 60 to 80 percent UDP packet loss.

That precise, repeated 984 Mbps ceiling is the real story here. It's essentially the practical achievable throughput of a 1 Gigabit Ethernet link once real world Ethernet, IP, and UDP overhead is accounted for, you never quite get the full 1000 Mbps in practice. Laptop 1's onboard NIC is a 1GbE chip, not capable of more, while the router's own NIC facing the Jetson is on higher capacity hardware. In other words, the Jetson was generating traffic faster than Laptop 1's physical cable could possibly carry it away, and the router correctly saturated that link right up to its real ceiling, with the excess unable to go anywhere and getting dropped. This is a genuine physical bandwidth mismatch between the two legs, not a flaw in our router's forwarding logic.

We confirmed this directly by throttling the Jetson to stay under that ceiling:

```bash
iperf3 -c 172.20.10.10 -u -b 900M -t 10 -i 1
```

Loss dropped close to zero at this rate, and reappeared as soon as we pushed back toward and past 1 Gbps, exactly what we'd expect if the explanation is correct.

## Step 10, improving the router's own efficiency regardless

Even though the physical link explains the loss we saw, we also found and fixed a genuine software inefficiency while investigating this. Our router was originally calling `rte_eth_tx_burst` once per single packet, which throws away most of the benefit of DPDK's burst oriented design, the entire reason DPDK can move packets so fast is by processing many at once, amortizing per call overhead across a batch. We restructured the forwarding path so that packets destined for the same output port during a single poll cycle are collected together and flushed with one real burst call, rather than many tiny ones. This doesn't remove the 1GbE ceiling, that's a hardware limit no amount of software efficiency changes, but it does mean we're now looking at the most honest, least software constrained picture of where the real limit sits.

## Step 11, actually seeing the packets, not just counting them

Throughput numbers tell us how much traffic moved, but not what was actually inside it. Since our DPDK bound ports have no kernel presence at all, ordinary tools like `tcpdump` cannot see anything on them, there's nothing for the kernel to hand to those tools. DPDK has its own proper mechanism for this, the `dpdk-pdump` tool, which attaches to a running DPDK application through EAL's own multi process support and pulls a real copy of the packets flowing through it into a standard pcap file.

We enabled this by calling `rte_pdump_init()` once near the start of our application, the only change needed on the app side. From a second terminal on the Ubuntu box, while the router kept running, we ran:

```bash
sudo dpdk-pdump -- --pdump 'port=0,queue=*,rx-dev=/tmp/laptop1.pcap'
```

and let it capture for a few seconds during an iperf3 run, then stopped it with Ctrl+C. We copied the resulting file back to our own laptop:

```
scp user_4@10.4.136.90:/tmp/laptop1.pcap .
```

and opened it directly in Wireshark, giving us genuine packet level inspection, real Ethernet, IP, and UDP or TCP headers, real payload bytes, of traffic that existed entirely in DPDK's own userspace memory the whole time, something no ordinary capture tool on this machine could have shown us any other way.

## Summary checklist

Physically wire the Jetson into the router's port that was previously used by another device.

Reach the Jetson over SSH through whatever separate management connection it already has, Wi-Fi in our case, never touch that connection's own configuration.

Identify the real wired interface with `ifconfig`, ignoring Docker bridges and USB gadget interfaces.

Assign a static IP to that wired interface, on a subnet confirmed not to collide with any real infrastructure already reachable from either machine.

Add explicit routes on both endpoints, since our clean router has no default route logic, every subnet crossing needs to be told about explicitly.

Build and run the router, and always confirm real port to device mapping from the live ARP learned log, never assume it from command line flag order.

Prove basic connectivity first with simple pings before generating any real load.

Generate controlled traffic with the Python script for simple, easily correlated tests, and with iperf3 for real sustained throughput measurement.

Read throughput ceilings carefully, a suspiciously precise, repeated number is often a real physical link limit, not a software bug, and can be confirmed by throttling the source below that ceiling and watching loss disappear.

Use `dpdk-pdump` and Wireshark for genuine packet content inspection, since ordinary kernel based tools cannot see anything on a vfio-pci bound interface at all.
