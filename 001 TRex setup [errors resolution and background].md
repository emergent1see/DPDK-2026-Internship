# TRex Traffic Generator Setup, Complete Documentation

## What we set out to do

After getting Pktgen-DPDK working on the Jetson as a traffic generator, we wanted to try TRex as a second, more sophisticated generator, this time running directly on the Ubuntu DPDK workstation itself rather than the Jetson. The goal was to generate traffic from a spare NIC on the workstation, have it physically loop back into our router's own port, let our router forward it exactly the way it already forwards Laptop 1's and the Jetson's traffic, and receive it on the Jetson at the far end. This document is the full story of getting there, including every error we hit and how we actually resolved each one.

## Checking what hardware we actually had free

Our router already owns two ports permanently, `0000:86:00.0` and `0000:d8:00.0`, both X540 10 Gigabit cards, one facing Laptop 1's old position and one facing the Jetson. Neither of these could be touched for this new generator, they needed to stay exactly as they were.

We ran `dpdk-devbind.py --status` and found a fuller picture of the workstation's hardware than we had been using so far. Beyond the two router ports, we had a second X540 port sitting idle, `0000:86:00.1`, on the same physical card as the router's own `86:00.0`, plus three separate I210 Gigabit cards, `0000:02:00.0`, `0000:04:00.0`, and `0000:af:00.0`, none of which were in use anywhere in our project.

Before touching `86:00.1`, we checked its IOMMU group, since it shares a physical card with the router's own port, and we wanted to be certain binding it separately wouldn't create any conflict. We found it in group 13, while the router's `86:00.0` sat in group 12, confirmed separate and safe to use independently.

## Downloading TRex, and a certificate problem

Our first attempt to download TRex failed outright.

```
ERROR: cannot verify trex-tgn.cisco.com's certificate
```

This was a certificate verification failure on our side, not a real problem with Cisco's server, most likely caused by an outdated local certificate trust store on this machine. We resolved it by adding `--no-check-certificate` to the download command, a reasonable tradeoff since we were fetching a known file from TRex's own official distribution domain, not something arbitrary. The download itself took a very long time on this network, over forty minutes for roughly 270 megabytes, but it did eventually complete successfully.

## The first real architectural wall, TRex needs paired interfaces

We ran TRex's own interactive setup tool, `dpdk_setup_ports.py -i`, expecting to simply select our one free port, `86:00.1`, and move on. Instead we learned something fundamental about how TRex is built. Unlike our own router code or Pktgen, both of which use DPDK's generic port API and treat every port independently, TRex's interactive tool insists on an even number of interfaces, structured around a client and server pairing model, even when running in stateless mode where that pairing doesn't conceptually apply the same way. Entering a single port number was flatly rejected with "Please specify an even number of interfaces."

This meant we needed a second interface purely to satisfy this requirement, even though nothing would ever be physically connected to it.

## The second wall, driver mismatch

Our first attempt at a second interface paired `86:00.1`, an X540 card using the `ixgbe` driver family, with `02:00.0`, an I210 card using the `igb` driver family. Writing this into `/etc/trex_cfg.yaml` by hand, since the interactive tool couldn't accommodate our single real interface plus dummy structure cleanly, we launched TRex and got a clear rejection.

```
ERROR all device should have the same type net_e1000_igb != net_ixgbe
```

This turned out to be a genuine, deliberate design constraint in TRex, not a bug or something a flag could bypass. We tried `--allow-coredump` specifically hoping it might relax this validation, it did not, since that flag only affects crash dump behavior and has nothing to do with driver matching at all. TRex's internal fast path code is tuned per driver family, and it refuses to start at all unless every port it manages shares the exact same underlying driver. Our own router and Pktgen never hit this limitation because DPDK's own generic API doesn't require this, TRex's is a stricter, more specialized design choice layered on top.

With only one free X540 port available, and every other X540 already committed elsewhere in our setup, this meant `86:00.1` could not be used at all for TRex, regardless of what we paired it with. We had to change our whole approach.

## The actual fix, two matching I210 ports

We moved to using `0000:af:00.0` as our real, working TRex port, and `0000:02:00.0` purely as the dummy pairing partner, never physically connected to anything. Both are I210 cards, both use the `igb` driver, satisfying TRex's matching requirement completely. We rewrote `/etc/trex_cfg.yaml` around this pair, with the real port's destination MAC set to the router's own port 0 MAC address, and the dummy port's fields left as harmless placeholders since nothing would ever actually transmit from it.

One consequence worth noting honestly, `af:00.0` is 1 Gigabit hardware, not the 10 Gigabit we'd originally hoped for with `86:00.1`. This caps whatever throughput we can eventually push through TRex specifically on this leg, a real, understood limitation rather than something to chase further, similar in nature to the 1GbE ceiling we found and confirmed with Laptop 1's own NIC earlier in this project.

## The physical loopback cable, and clearing up what it actually connects

We want to be precise here since this caused some real confusion partway through. Only one physical cable exists in this entire generator setup, running from `af:00.0`'s port directly into the router's `86:00.0` port, the same physical port Laptop 1 used to occupy. This is a genuine loopback, two ports on the same machine wired together, and it's what lets a packet TRex generates physically leave through `af:00.0` and arrive at the router's own receiving port. The second interface, `02:00.0`, is never cabled to anything at all, it exists purely as a formality satisfying TRex's pairing requirement, and connecting it to anything, including `af:00.0` itself, would be incorrect and unnecessary.

## The scapy server permission error

Our first launch attempt with a valid config still failed, this time with a permissions complaint about TRex's internal scapy server, which it uses for GUI based packet construction, unable to read our home directory as the lower privilege `nobody` user. Since we were driving TRex entirely through its console commands rather than its graphical packet builder, we resolved this simply by adding `--no-scapy-server` to the launch command, skipping that component entirely rather than restructuring file permissions or moving the whole TRex installation to a public directory as the alternative suggestion would have required.

## The EAL lock conflict between TRex and our router

Once TRex was running, launching our router in a separate session failed immediately.

```
EAL: Cannot create lock on '/var/run/dpdk/rte/config'. Is another primary process running?
```

Both DPDK applications were defaulting to the same internal lock and shared memory file, even though they owned completely separate PCI devices. We resolved this by giving the router its own explicit file prefix, `--file-prefix=router`, on its launch command, which tells EAL to use separate lock and shared memory files distinct from whatever TRex was using by default. Only the newly launching process needed this change, TRex, already running first, kept working unmodified.

## Confirming which physical interface plays which role

With everything running simultaneously, we made sure to write down exactly which named interface did what, since with five different NICs now involved across this project it became easy to lose track.

`enp134s0f0`, PCI `0000:86:00.0`, the router's port 0, now receiving TRex's generated traffic.

`enp216s0f0`, PCI `0000:d8:00.0`, the router's port 1, still facing the Jetson exactly as it always has, completely unchanged by any of this TRex work.

`enp175s0`, PCI `0000:af:00.0`, TRex's real generating port, cabled directly into the router's `86:00.0`.

`eno1` at PCI `0000:02:00.0`, TRex's dummy pairing partner, bound but uncabled and inert.

## The vfio-pci kernel module not loaded

At one point, attempting to rebind our TRex ports produced this error.

```
Warning: no supported DPDK kernel modules are loaded
Error: Driver 'vfio-pci' is not loaded.
```

The `vfio-pci` kernel module had simply dropped out of memory at some point during our long session, likely tied to the machine sitting idle or an intervening reboot, the same category of issue we had already run into earlier in this project with hugepage reservations not surviving between sessions. The fix was a single command, `sudo modprobe vfio-pci`, run again before any further binding attempts.

## The stream profile script error

Our first attempt at writing a custom TRex stream profile, meant to generate real UDP packets matching our own addressing scheme rather than TRex's generic sample script, failed to load.

```
AttributeError: module 'our_udp_stream' has no attribute 'register'
```

We had written the script with a bare module level variable holding our stream class instance, but TRex's profile loader specifically calls a function named `register()` at the module level and expects it to return the stream object. We corrected the script to define `register()` properly, returning an instance of our stream class, which resolved the loading error immediately.

## The persistent queue full crash, and its real root cause

Even with a correctly loading stream and traffic reportedly started successfully from the console, the TRex server process itself crashed repeatedly, always with the same signature. The `Total_queue_full` counter climbed into the hundreds of thousands, sometimes over a million, within seconds, `opackets` stayed at exactly zero the entire time, and eventually TRex's own internal watchdog killed the data plane core with a timeout, since it had been stuck spinning trying to flush a transmit queue that was never draining at all.

We worked through several possible explanations in order. We confirmed the router itself was genuinely running the whole time, ruling out the simplest explanation. We confirmed both TRex owned ports showed up correctly bound in `dpdk-devbind.py --status`. What we eventually noticed, looking carefully back at TRex's own very first startup message from much earlier in this process, was a line we had glossed over.

```
Trying to bind to vfio-pci ...
Trying to bind to try_bind_to_uio_pci_generic ...
```

TRex had been silently falling back to `uio_pci_generic` on its own, overriding whatever driver we had manually bound beforehand, every single time it started. `uio_pci_generic` is an older, simpler userspace I/O mechanism that does not use the IOMMU at all, and on certain hardware combinations it has known issues correctly initializing descriptor rings and DMA, which lines up exactly with what we were seeing, packets being accepted into software queues but never actually reaching the wire through hardware.

We forced this explicitly, unbinding both TRex ports completely, reloading the `vfio-pci` module fresh, and rebinding both ports to `vfio-pci` by hand immediately before relaunching TRex, rather than letting its own setup logic choose. On the next launch, the `Trying to bind to vfio-pci ... falling back` messages did not appear at all, suggesting our explicit binding held this time rather than being silently overridden.

## Where we currently stand

We are in the process of confirming whether forcing `vfio-pci` binding resolves the queue full crash entirely, watching for the port to report a genuine link up state and real climbing packet counts on both the TRex server's own statistics and the router's forwarding stats, rather than the persistent zero output and crash we saw on every attempt before this fix. This is the last open thread in getting TRex fully working end to end through our router to the Jetson, and the next step is confirming a clean, sustained run with real traffic actually arriving and visible on the receiving end.
