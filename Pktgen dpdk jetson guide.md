# Pktgen-DPDK Setup Guide, Running on the Jetson AGX Orin

## What we're building

We want the Jetson itself to run Pktgen-DPDK, a real hardware speed traffic generator built directly on top of DPDK, rather than an ordinary Linux program like `iperf3`. Traffic generated this way bypasses the Jetson's own kernel network stack entirely, gets sent out at true line rate, arrives at our DPDK router on the Ubuntu box, gets routed exactly like any other traffic in our scheme, and lands on Laptop 1.

```
Jetson (Pktgen-DPDK)  --->  Ubuntu DPDK Router  --->  Laptop 1
192.168.30.10/24 leg       Port1=.30.1, Port0=172.20.10.1     172.20.10.10/24
```

## Step 1, find out exactly what NIC chip the Jetson actually has

This decides which of two approaches we take, and it's worth checking properly rather than guessing.

```bash
lspci -nn | grep -i ethernet
```

Note the vendor and device ID shown in brackets, something like `[10ec:8168]` for a Realtek chip, or `[8086:1533]` for an Intel one.

Check whether DPDK ships a poll mode driver for that specific chip:

```bash
~/dpdk/usertools/dpdk-devbind.py --status
```

Most Jetson dev kit onboard Ethernet controllers are consumer grade Realtek chips, and DPDK's mainline poll mode drivers are generally written for enterprise and datacenter NICs, Intel's ixgbe and i40e families, Mellanox, Broadcom, and similar, not consumer Realtek silicon. If that's what we find here, a genuine hardware poll mode driver with `vfio-pci` binding, the same approach we used on the Ubuntu box, will not work on the Jetson's onboard port.

## Step 2, the realistic path for most Jetson boards, DPDK's AF_XDP or AF_PACKET virtual device

If the onboard chip has no native DPDK driver, which is the common case, DPDK still gives us a working path, a virtual device type that uses the existing kernel network interface as its actual I/O path underneath, while still presenting a normal DPDK port to our application above it. This sacrifices some of the absolute peak throughput a true bypass driver would give us, but it works on essentially any NIC with zero dependency on vendor specific driver support, and is still dramatically faster and more controllable than a plain socket based tool like `iperf3`.

We'll use `AF_XDP` if the Jetson's kernel supports it, checked with:

```bash
uname -r
```

Kernel 4.18 or newer generally has AF_XDP support built in. If it's an older kernel, we fall back to `AF_PACKET` instead, both work the same way from our application's perspective, just with different underlying performance characteristics.

## Step 3, install build dependencies on the Jetson

```bash
sudo apt update
sudo apt install -y build-essential meson ninja-build python3-pyelftools python3-pip libnuma-dev pkg-config git linux-headers-$(uname -r) net-tools pciutils libpcap-dev liblua5.3-dev libreadline-dev
```

## Step 4, build DPDK natively on the Jetson

This is a separate build from the Ubuntu box's own DPDK install, since these are two different machines with two different CPU architectures, the Jetson is ARM64, the Ubuntu box is x86_64. We build DPDK directly on the Jetson itself, no cross compilation needed, since we're compiling natively for the architecture we're actually running on.

```bash
cd ~
git clone https://github.com/DPDK/dpdk.git
cd dpdk
meson setup build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

## Step 5, reserve huge pages on the Jetson

The Jetson AGX Orin has unified memory shared between CPU and GPU, and a single NUMA node, so this is simpler than the multi socket reservation we did on the Ubuntu box.

```bash
echo 512 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
grep -i huge /proc/meminfo
sudo mkdir -p /mnt/huge
sudo mount -t hugetlbfs nodev /mnt/huge
```

512 pages at 2MB each gives us 1GB, comfortably enough for Pktgen's own packet buffers.

## Step 6, build Pktgen-DPDK

```bash
cd ~
git clone https://github.com/pktgen/Pktgen-DPDK.git
cd Pktgen-DPDK
meson setup build
ninja -C build
```

Confirm the binary exists:

```bash
find . -name "pktgen" -type f
```

## Step 7, launch Pktgen using AF_PACKET

This is where we bridge the kernel's `eno1` interface, the one physically wired to our Ubuntu DPDK router, into a DPDK port Pktgen can use.

We tried `AF_XDP` first, but our DPDK build didn't have it compiled in, EAL reported it couldn't parse `net_af_xdp0` at all, which happens when the `libxdp` and `libbpf` development headers weren't present at DPDK build time. `AF_PACKET` picked up correctly, so that's the path this guide actually uses.

We also ran into a real driver quirk worth knowing about before launching. NVIDIA's `nvethernet` driver, which controls this interface, refuses to accept an MTU change while the interface is administratively up, it fails with `must be stopped to change its MTU`. Since Pktgen tries to set the MTU as part of its own startup sequence, we need `eno1` already down at that moment, and we leave it down through the launch itself, rather than bringing it back up first:

```bash
sudo ip link set eno1 down
sudo ip link set dev eno1 mtu 1500
```

No IP address is needed on `eno1` at all, and none should be set. `AF_PACKET` operates at the raw Ethernet frame level, Pktgen builds every packet itself, Ethernet header, IP header, everything, entirely in its own memory, and hands the finished frame straight to the interface. The kernel's own IP configuration on `eno1` never enters into this.

Launch Pktgen with the interface still down:

```bash
cd ~/Pktgen-DPDK
sudo ./build/app/pktgen -l 0-2 -n 4 --socket-mem=512 \
  --vdev=net_af_packet0,iface=eno1 -- -P -m "1.0"
```

```bash
sudo ./build/app/pktgen -l 0-2 -n 4 --socket-mem=512 \
  --vdev=net_af_packet0,iface=eno1 -- -P -m "1.0"
```

This drops us into Pktgen's own interactive console, a Lua scriptable command prompt built specifically for configuring and firing traffic generation.

## Step 8, configure the packet we want to generate, inside Pktgen's console

We need Pktgen to build packets that our DPDK router will actually recognize and forward, meaning the destination IP needs to be Laptop 1's real address, and the destination MAC needs to be our router's own MAC address on the Jetson facing port, since Pktgen operates below the kernel entirely and does not do its own ARP resolution automatically unless we tell it to.

First, get the router's Jetson facing MAC address, it printed this in its own startup banner earlier when we launched `jetson_router`, look for the line showing Port 1's MAC.

Inside the Pktgen console:

```
set 0 src ip 192.168.30.10
set 0 dst ip 172.20.10.10
set 0 dst mac <router's port 1 MAC address here>
set 0 size 128
set 0 rate 50
```

`set 0 src ip` is the Jetson's own generator address, matching our existing scheme. `set 0 dst ip` is Laptop 1's real address, this is what our router's routing table lookup will match against to decide where to forward. `set 0 dst mac` is the crucial piece letting the packet actually leave the Jetson correctly, since Ethernet delivery on the local wire happens by MAC address, not IP, and without setting this explicitly Pktgen has no way to know who to hand the frame to. `set 0 rate` is a percentage of line rate, `50` meaning half of whatever this virtual interface can achieve.

# Using Lua for automation

Every setting we've been typing, protocol, rate, packet size, destination IP and MAC, only exists in Pktgen's memory while it's running. The moment the process exits, whether from a crash, a deliberate stop, or closing the terminal, all of that configuration is gone, and a fresh launch starts from Pktgen's own defaults again, exactly why you've seen the port bounce back to TCP and the destination MAC reset to zeros each time we relaunched.

## The fix, a Lua startup script

Pktgen is built specifically around Lua scripting, and it can load a script automatically at launch, running every command in it before dropping you into the interactive console, so all our configuration happens instantly and consistently every time, with zero manual retyping.

Create the script on the Jetson:

```bash
cat > ~/Pktgen-DPDK/setup.lua << 'EOF'
pktgen.set_proto("0", "udp");
pktgen.set("0", "rate", 100);
pktgen.set("0", "size", 1500);
pktgen.set_ipaddr("0", "src", "192.168.30.10");
pktgen.set_ipaddr("0", "dst", "172.20.10.10");
pktgen.set_mac("0", "b4:96:91:12:ab:d0");
pktgen.dst_port("0", 9999);
EOF
```

Replace the MAC address on that last relevant line with your router's actual Port 1 MAC, same one you already retrieved earlier.

## Launch with the script loaded automatically

```bash
sudo ip link set eno1 down
sudo ip link set dev eno1 mtu 1500
sudo ./build/app/pktgen -l 0-2 -n 4 --socket-mem=512 --vdev=net_af_packet0,iface=eno1 -- -P -m "2.0" -f setup.lua
```

The `-f setup.lua` flag tells Pktgen to execute that file automatically right after it initializes, before you even see the console, so by the time it's ready, every setting is already exactly where we want it. You'll still need the separate `sudo ip link set eno1 up` in a second terminal once it's running, since that's an operating system level step outside Pktgen's own scripting entirely, and `start 0` inside the console when you're ready to actually begin sending, since that's a deliberate action you'd want to control manually rather than have fire automatically the instant it launches.

Try this, and from here on, launching is just the one command with `-f setup.lua` attached, no manual retyping needed each time.

## Step 9, start generating

```
start 0
```

Traffic now flows continuously out of the Jetson, into our DPDK router, and if everything is configured correctly, onward to Laptop 1. Watch Pktgen's own live display for its transmitted packet and byte counters.

## Step 10, see it, three places at once

Watch the router's own console on the Ubuntu box, our per second throughput stats will show port 1's RX climbing as this traffic arrives, and port 0's TX climbing as it gets forwarded onward.

On Laptop 1, since Pktgen is sending raw crafted UDP packets rather than a formatted protocol `iperf3` understands, the simplest way to actually see arrival is a packet capture directly on Laptop 1's own interface, since that's a completely normal kernel interface. Run Wireshark there directly, filtering on `ip.src == 192.168.30.10`, and you should see the packets arriving live as Pktgen fires them.

Alternatively, for a simpler numeric confirmation, a basic listener on whatever destination port Pktgen is using, default is UDP port `9`, works too:

```
nc -ul 9
```
though note it won't print anything readable, since these are Pktgen's own raw generated payloads, not text, this is really just to confirm packets are landing rather than to read their contents meaningfully.

## Step 11, stop the test

```
stop 0
```

inside the Pktgen console, and check its final summary, total packets and bytes sent, alongside whatever the router's own stats showed as forwarded, comparing the two tells us directly whether anything was lost between the Jetson's generation and the router's own reception.

## We did not saw anything on laptop by running the script Why nothing was reaching Laptop 1

Look at this line:
```
>>> User State for CLI not set for Lua, please build with Lua enabled
```

This build of Pktgen doesn't have Lua scripting compiled in, so our `setup.lua` file, written using Lua function calls like `pktgen.set_ipaddr(...)`, silently failed to actually apply almost anything. Only the MAC address stuck, likely from an earlier session's leftover state, everything else quietly stayed at Pktgen's own defaults.

That's the whole story right there. Look at the port info still showing `IP Destination : 192.168.1.1`, that's Pktgen's built in default, never actually changed to Laptop 1's real address, `172.20.10.10`. Since `192.168.1.1` doesn't match either subnet our router actually knows about, the router correctly sees it as unroutable and silently drops every single packet, exactly matching what we see, port 1's RX climbing nicely as the Jetson generates traffic, but port 0's TX sitting frozen at zero the entire time, since there's genuinely nowhere for the router to send it.

Also, unrelated but worth fixing, `set 0 dst port 9999` isn't valid syntax, Pktgen's actual command is `dport`, not `dst port`.

## Fix everything manually, right now, typed directly into the console

Since scripting isn't available in this build, we set each value by hand this one time:

```
stop 0
clear all
set 0 proto udp
set 0 size 1500
set 0 rate 100
set 0 src ip 192.168.30.10
set 0 dst ip 172.20.10.10
set 0 dst mac b4:96:91:12:ab:d0
set 0 dport 9999
```

`clear all` is the command you were actually looking for, it zeroes every counter and statistic back to a clean slate, exactly what you want before starting a fresh Wireshark capture.

## Confirm before starting

Check the live port info display, it should now show `Type:VLAN ID:Flags : IPv4 / UDP`, and `IP Destination : 172.20.10.10`, not the old `192.168.1.1` default. Don't start generating until both of those are correct.

## Then start, and set up Wireshark

```
start 0
```

On Laptop 1, in Wireshark, capturing on the Ethernet adapter facing the router:
```
ip.src == 192.168.30.10 and udp.port == 9999
```

Watch the router console at the same time now, port 0's TX should finally start climbing alongside port 1's RX, that's the confirmation the destination fix actually worked.

## For next time, a working alternative to scripting

Since this build lacks Lua, file based automation genuinely isn't available to us here unless Pktgen gets rebuilt with Lua support enabled, a build time option we could look into separately if retyping this each session becomes too tedious. For now, keeping this exact block of commands saved in a text file on the Jetson, ready to copy and paste into the console after each launch, is the realistic middle ground.
## A note on realistic expectations

Since we're going through the `AF_PACKET` path rather than a genuine hardware poll mode driver, the Jetson's own kernel networking stack is still involved underneath, at a much lower level than a normal socket application, but not fully bypassed the way our Ubuntu box's `vfio-pci` bound ports are. This means the true line rate ceiling here is set by how fast the Jetson's kernel driver and this virtual device layer can move frames, not necessarily the full physical capability of the NIC chip itself, which we confirmed during setup is a genuine 10 Gigabit link. This is still a genuinely more capable, more controllable generator than `iperf3`, since we're crafting exact packets and pushing them through DPDK's own transmit path, just worth knowing this specific number won't be a pure hardware benchmark the way our earlier tests on the Ubuntu box's real 10G cards were.
