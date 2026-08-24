# TRex Setup Guide, Complete Walkthrough

## What we are building

We want a real hardware speed traffic generator, TRex, running directly on our Ubuntu DPDK workstation, sending traffic that physically loops back into our own router application, gets forwarded exactly the way any other traffic on our network does, and arrives on the Jetson AGX Orin at the far end.

```
TRex (spare NIC)  ==cable==>  Router port 0  --forwarded internally-->  Router port 1  ==cable==>  Jetson
```

The router itself, `jetson_router`, does not change at all for this. It has no idea whether traffic arriving on its port 0 came from a real laptop, from Pktgen, or from TRex, it simply sees packets and forwards them according to its own routing table, same as always. That is the entire point of building it this way, TRex becomes just another traffic source feeding into a pipeline we already trust.

## Step 1, take stock of what hardware we actually have

Our router permanently owns two ports, `0000:86:00.0` facing what we treat as the Laptop 1 leg, and `0000:d8:00.0` facing the Jetson. Neither of these can be touched for TRex, they need to keep doing exactly what they already do.

```bash
sudo dpdk-devbind.py --status
```

We look for genuinely idle NICs beyond these two. On our workstation we found a second X540 port sharing a card with the router's own `86:00.0`, and three separate I210 Gigabit cards, none of which were in use anywhere in our project.

## Step 2, understand TRex's specific hardware requirement before choosing ports

This is the part worth understanding clearly before picking anything, since it shapes every choice that follows. Unlike our own router code and Pktgen, both of which use DPDK's generic port API and treat every port completely independently, TRex enforces two extra rules of its own.

It requires an even number of interfaces, structured internally around a client and server pairing model, even in stateless mode where that pairing doesn't conceptually matter the same way.

It requires every port it manages to run the exact same underlying driver. Mixing an X540 card, driver `ixgbe`, with an I210 card, driver `igb`, causes TRex to refuse outright at startup with an explicit driver mismatch error. This is a deliberate design choice in TRex's own fast path code, not a bug, and no command line flag overrides it.

Given we only have one genuinely free X540 port, and every I210 is idle, the only workable pairing is two I210 ports together. One becomes our real, working port, physically cabled to the router. The other exists purely to satisfy TRex's pairing requirement and is never connected to anything at all.

We check IOMMU groups before committing to any port that shares a physical card with something already in use, confirming no overlap:

```bash
for d in /sys/kernel/iommu_groups/*/devices/*; do
  g=$(echo $d | grep -oP 'iommu_groups/\K[0-9]+')
  echo "Group $g: $(basename $d)"
done | grep -E "86:00.0|86:00.1|af:00.0|02:00.0"
```

## Step 3, our final port assignment

`0000:af:00.0`, `enp175s0`, our real TRex port, physically cabled directly into the router's `86:00.0`.

`0000:02:00.0`, our dummy pairing partner, bound to DPDK but never cabled to anything.

`0000:86:00.0`, the router's port 0, unchanged, now receiving TRex's traffic.

`0000:d8:00.0`, the router's port 1, unchanged, still facing the Jetson.

## Step 4, load the kernel module and bind explicitly

We always load `vfio-pci` fresh and bind by hand, rather than trusting any automatic tool to choose the right driver for us. This matters more than it might seem, TRex's own setup logic can silently fall back to a different, less capable binding mechanism on its own if it isn't given an already correctly bound device to work with, so we make sure this step is done properly before TRex ever touches these ports.

```bash
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:af:00.0
sudo dpdk-devbind.py --bind=vfio-pci 0000:02:00.0
sudo dpdk-devbind.py --status
```

We confirm both show `drv=vfio-pci` specifically before moving on.

## Step 5, the physical loopback cable

One single cable, running from `af:00.0`'s physical port directly into the router's `86:00.0` port, the same port Laptop 1 used to occupy. This is a genuine, real Ethernet connection, two ports on the same machine wired together, not a simulation of any kind. `02:00.0` stays completely uncabled, it never carries a single real packet.

## Step 6, download TRex

```bash
cd ~
wget --no-check-certificate https://trex-tgn.cisco.com/trex/release/latest
tar -xzf latest
cd v*
```

We use `--no-check-certificate` here since this specific machine's certificate trust store doesn't verify Cisco's server chain cleanly, a reasonable tradeoff for a known, official download source. This transfer can take a while depending on connection speed, the archive itself is around 270 megabytes.

## Step 7, write the TRex configuration file directly

TRex's own interactive setup tool, `dpdk_setup_ports.py -i`, is built around selecting matched pairs from a menu, and it does not handle our specific situation cleanly, one real port and one deliberately uncabled placeholder. We write `/etc/trex_cfg.yaml` by hand instead, which is actually the more precise approach for this topology.

First, get the router's own port 0 MAC address, it prints this in its own startup banner every time it launches, look for the `Port 0` line.

```bash
sudo nano /etc/trex_cfg.yaml
```

```yaml
- version: 2
  interfaces: ['af:00.0', '02:00.0']
  port_info:
    - dest_mac: '<router's actual port 0 MAC here>'
      src_mac: '<af:00.0's own MAC, shown in dpdk-devbind.py --status>'
    - dest_mac: '<02:00.0's own MAC>'
      src_mac: '<02:00.0's own MAC>'
  platform:
    master_thread_id: 0
    latency_thread_id: 1
    dual_if:
      - socket: 1
        threads: [2, 3]
```

The first `port_info` entry is our real port. Its `dest_mac` is the router's address, since that's who we're actually trying to reach across the cable. The second entry is the placeholder, its values genuinely don't matter since nothing ever transmits from it, we simply reuse its own MAC as a harmless filler so the file stays valid.

## Step 8, launch the TRex server process

```bash
cd ~/v3.08
sudo ./t-rex-64 -i --cfg /etc/trex_cfg.yaml --no-scapy-server
```

We add `--no-scapy-server` since TRex's built in scapy component, used for its graphical packet builder, runs as a lower privilege system user that cannot read into our home directory by default. Since we are driving TRex entirely through its own console rather than a GUI, we don't need this component at all.

We watch the startup output carefully here. If we see `Trying to bind to vfio-pci ... Trying to bind to uio_pci_generic`, that means TRex is falling back to a less capable binding mechanism on its own, one that lacks IOMMU protection and has caused real transmit failures for us in the past. If this appears, we stop, unbind both ports, reload `vfio-pci`, rebind explicitly again, and relaunch before continuing any further.

## Step 9, launch our router, with its own distinct identity

Since TRex and our router are two separate DPDK processes running at the same time, each needs its own internal lock file, otherwise the second one to start fails immediately with a lock conflict.

```bash
cd ~/dpdk/code-files
sudo ./jetson_router -l 10,11,12 -n 4 --socket-mem=0,512 -a 0000:86:00.0 -a 0000:d8:00.0 --file-prefix=router
```

We confirm the full startup banner appears and the process stays running, this is also where we read the Port 0 MAC address used back in Step 7 if we hadn't already.

## Step 10, write our own traffic stream profile

TRex's sample scripts use generic placeholder addresses, we want our own, matching our actual scheme, TRex standing in for the old Laptop 1 position, sending toward the Jetson's real address.

```bash
cat > ~/v3.08/stl/our_udp_stream.py << 'EOF'
from trex_stl_lib.api import *

class STLS1(object):
    def create_stream(self):
        pkt = Ether(dst="<router's port 0 MAC>") / \
              IP(src="172.20.10.10", dst="192.168.30.10") / \
              UDP(sport=1234, dport=9999) / \
              (b'X' * 100)
        return STLStream(packet=STLPktBuilder(pkt=pkt), mode=STLTXCont())

    def get_streams(self, direction=0, **kwargs):
        return [self.create_stream()]

def register():
    return STLS1()
EOF
```

The `register()` function at the end matters specifically, TRex's own script loader looks for exactly this name at the module level and calls it to obtain our stream object, a script without it will fail to load with an attribute error.

## Step 11, connect the console and start generating

```bash
sudo ./trex-console
```

```
start -f stl/our_udp_stream.py -m 10mbps -p 0
tui
```
**The tui console will output like this**
```bash
Global Statistics

connection   : localhost, Port 4501                       total_tx_L2  : 10.01 Mbps
version      : STL @ v3.08                                total_tx_L1  : 11.38 Mbps
cpu_util.    : 0.03% @ 1 cores (1 per dual port)          total_rx     : 0 bps
rx_cpu_util. : 0.0% / 0 pps                               total_pps    : 8.57 Kpps
async_util.  : 0% / 2.17 bps                              drop_rate    : 10.01 Mbps
total_cps.   : 0 cps                                      queue_full   : 0 pkts

Port Statistics

   port    |         0         |         1         |       total
-----------+-------------------+-------------------+------------------
owner      |              root |              root |
link       |                UP |              DOWN |
state      |      TRANSMITTING |              IDLE |
speed      |            1 Gb/s |             0 b/s |
CPU util.  |             0.03% |              0.0% |
--         |                   |                   |
Tx bps L2  |        10.01 Mbps |             0 bps |        10.01 Mbps
Tx bps L1  |        11.38 Mbps |             0 bps |        11.38 Mbps
Tx pps     |         8.57 Kpps |             0 pps |         8.57 Kpps
Line Util. |            1.14 % |               0 % |
---        |                   |                   |
Rx bps     |             0 bps |             0 bps |             0 bps
Rx pps     |             0 pps |             0 pps |             0 pps
----       |                   |                   |
opackets   |           6242389 |                 0 |           6242389
ipackets   |                 0 |                 0 |                 0
obytes     |         911388794 |                 0 |         911388794
ibytes     |                 0 |                 0 |                 0
tx-pkts    |        6.24 Mpkts |            0 pkts |        6.24 Mpkts
rx-pkts    |            0 pkts |            0 pkts |            0 pkts
tx-bytes   |         911.39 MB |               0 B |         911.39 MB
rx-bytes   |               0 B |               0 B |               0 B
-----      |                   |                   |
oerrors    |                 0 |                 0 |                 0
ierrors    |                 0 |                 0 |                 0

status:  \

Press 'ESC' for navigation panel...
status:

tui>
```
We start conservatively, well under any bandwidth ceiling we might hit, purely to confirm the whole path genuinely works before pushing higher. The `tui` command switches to a live statistics view, we watch port 0's `opackets` and `Tx Bw` here, real climbing numbers confirm packets are actually leaving the port, not just being accepted into a software queue.

## Step 12, confirm arrival on the Jetson

```bash
ssh agx-1@10.1.81.191
sudo tshark -i eno1 -f "src host 172.20.10.10"
```

This filters specifically for traffic whose source matches TRex's generating address, confirming packets are genuinely arriving at the far end, completing the full chain from TRex, through the router, to the Jetson.

## A short troubleshooting reference, for the issues most likely to recur

If TRex's config validation refuses two ports with different driver names, that means an X540 and an I210 have been paired together, replace one so both ports share the same driver family.

If the TRex server process crashes with a `WATCHDOG` timeout and a rapidly climbing `Total_queue_full` counter while `opackets` stays at zero, this is almost always TRex having silently fallen back to `uio_pci_generic` instead of `vfio-pci`. Unbind, reload the kernel module, and rebind explicitly before relaunching.

If launching the router while TRex is already running fails with a lock error on `/var/run/dpdk/rte/config`, add `--file-prefix=router` to the router's own launch command.

If a custom TRex stream script fails to load with an attribute error mentioning `register`, the script is missing its `register()` function, TRex's loader specifically requires this exact entry point.

If `dpdk-devbind.py` reports the `vfio-pci` driver is not loaded at all, the kernel module has simply dropped out since the last session, `sudo modprobe vfio-pci` resolves it immediately.

## We are unable to receive packets on the other end. 
