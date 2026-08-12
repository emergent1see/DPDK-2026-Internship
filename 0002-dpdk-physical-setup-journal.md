# DPDK Physical Setup Journal

## What we built

We moved our DPDK L3 router project off VirtualBox and onto real hardware. The setup is a bare metal Ubuntu machine, a Lenovo TC-S05 dual socket Xeon workstation, sitting in the middle of two laptops. Each laptop runs a VM (VM1 and VM2), and each laptop is physically wired via Ethernet cable into a separate NIC on the Ubuntu box. The Ubuntu machine runs our DPDK application and acts as the router between the two laptops.

We call this topology VM1, then the DPDK router in the middle, then VM2. Two legs, two subnets, one router doing real Layer 3 forwarding between them.

## Why we left VirtualBox

We first tried to get DPDK's IOMMU/VFIO path working entirely inside a VirtualBox VM. This meant enabling VirtualBox's own virtual IOMMU emulation, which required switching the VM's chipset from PIIX3 to ICH9 (VirtualBox only emulates IOMMU on ICH9), enabling nested hardware virtualization, and setting `--iommu automatic` through VBoxManage.

Along the way we hit several rough edges:

We tried `VBoxManage modifyvm --iommu-type automatic` and got an unknown option error. The correct flag turned out to be `--iommu`, not `--iommu-type`.

We then hit `VBOX_E_INVALID_VM_STATE` errors because the VM was in a saved state rather than fully powered off. VirtualBox will not let you change chipset or hardware virtualization settings on a saved state VM. We had to discard the saved state first with `VBoxManage discardstate` before the chipset and IOMMU changes would apply.

After the chipset change from PIIX3 to ICH9, the VM booted and asked for an ISO file instead of booting into our installed Ubuntu system. We were worried we had lost everything. It turned out the chipset swap had changed how the storage controller was wired, and the VM's SATA controller showed no disk attached in the Storage settings, even though the IDE controller still correctly showed our installer ISO. We searched the VM's folder and the whole filesystem for the `.vdi` disk file and never found it in the expected location, which left us uncertain whether the disk data was truly gone or just misconfigured. We ultimately decided not to keep chasing this and moved to bare metal instead, since we wanted a cleaner and more reliable environment anyway.

## Moving to real hardware

We identified our two physical DPDK facing NICs on the Ubuntu machine:

`enp134s0f0`, PCI address `0000:86:00.0`, wired to the laptop running VM1

`enp216s0f0`, PCI address `0000:d8:00.0`, wired to the laptop running VM2

We confirmed this mapping by watching `ip link show` while physically unplugging each cable one at a time and noting which interface dropped its carrier signal. This is the most reliable way to map a cable to an interface when multiple ports look similar.

We made sure to identify our management interface separately, since binding the wrong NIC to DPDK on real hardware means losing our SSH session with no console fallback.

## Verifying IOMMU on real hardware

Unlike the VirtualBox emulation, bare metal VT-d worked cleanly right away. We checked with `dmesg | grep -e DMAR -e IOMMU` and got clear confirmation. Our machine has eight separate DMAR/IOMMU units, which told us this is a genuine multi socket server class board, not a simple desktop. We also saw two NUMA proximity domains, zero and one, confirming the dual socket layout.

We checked which IOMMU group each of our two NICs belongs to:

`0000:86:00.0` sits in IOMMU group 12

`0000:d8:00.0` sits in IOMMU group 7

Different groups meant we could bind each independently to `vfio-pci` with no risk of pulling in unrelated devices.

We also checked NUMA locality for each NIC:

```
cat /sys/bus/pci/devices/0000:86:00.0/numa_node
cat /sys/bus/pci/devices/0000:d8:00.0/numa_node
```

Both came back as node 1. This was good news, since it meant we could pin our DPDK polling cores and huge page memory entirely to node 1 and avoid any cross socket memory latency. We found node 1's local core range with `cat /sys/devices/system/node/node1/cpulist`, which gave us cores 10 through 19 and 30 through 39. We chose cores 10, 11, and 12 for our testpmd and router launches, one for the main lcore and one each for polling the two ports.

## Installing DPDK

We installed build dependencies, cloned the DPDK source, and built it with meson and ninja:

```
sudo apt install -y build-essential meson ninja-build python3-pyelftools python3-pip libnuma-dev pkg-config git linux-headers-$(uname -r) net-tools pciutils
git clone https://github.com/DPDK/dpdk.git
cd dpdk
meson setup build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

We confirmed the install with `dpdk-testpmd --version` and `pkg-config --modversion libdpdk`, both of which reported DPDK 26.11.0-rc0.

## Binding the NICs

We loaded the vfio-pci kernel module and bound both NICs:

```
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:86:00.0
sudo dpdk-devbind.py --bind=vfio-pci 0000:d8:00.0
```

At one point after a reboot, testpmd reported it could not find any probed ethernet devices at all, even though we had bound both NICs earlier. This happened because the binding does not persist across a reboot on its own, and we had to run the bind commands again after the machine came back up. Since then we treat rechecking `dpdk-devbind.py --status` as a mandatory first step every session.

## Hugepage troubles

Our first attempt to run testpmd failed with permission denied errors trying to open hugepage files under `/dev/hugepages`. Running testpmd with sudo fixed this, since DPDK genuinely needs root level access to hugepage memory and raw hardware.

Our next attempt failed differently. The mbuf pool creation failed with a cannot allocate memory error, because testpmd by default tries to allocate from NUMA socket 0, and we had only reserved hugepages on node 1 where our NICs actually live. We fixed this by explicitly setting `--socket-mem=0,1024` so DPDK requests zero megabytes from socket 0 and the full amount from socket 1.

We then hit a further variant of the same issue, an error saying not enough memory available on socket 1, requesting 1024 megabytes but only 512 available. Our earlier hugepage reservation had not fully landed, likely due to memory fragmentation happening after boot, which can prevent the kernel from finding enough physically contiguous 2MB blocks at runtime. We resolved this two ways, either lowering our request to match what was actually available with `--socket-mem=0,512`, or reserving hugepages properly at boot time through GRUB parameters so the reservation happens before anything else fragments memory.

## The IP scheme

We designed our addressing so that VM1 and VM2 sit on genuinely different subnets, which forces the Ubuntu box to do real routing rather than simple bridging.

VM1's wired interface gets `192.168.10.10/24`

Port 0 on the Ubuntu router, facing VM1, represents `192.168.10.1`

Port 1 on the Ubuntu router, facing VM2, represents `192.168.20.1`

VM2's wired interface gets `192.168.20.10/24`

We had to be clear with ourselves that the Ubuntu router's two DPDK bound ports have no kernel level IP address at all, since binding to vfio-pci detaches them from the Linux network stack entirely. There is no `ip addr add` command that applies to them. Instead these addresses exist only as values inside our DPDK application's own source code, which we use for answering ARP requests and making routing decisions.

On the VM side, the IPs are real kernel level configuration:

```
sudo ip addr add 192.168.10.10/24 dev <VM1 wired interface>
sudo ip link set <VM1 wired interface> up
sudo ip route add default via 192.168.10.1
```

and the mirrored version on VM2 with `192.168.20.10/24` and a default route via `192.168.20.1`.

## Writing the actual router

Our earlier code, `bidirectional_listening.c`, only did raw Ethernet frame forwarding between two ports with no awareness of IP addresses at all. It was useful as a first sanity check but was not a real router.

We wrote `l3_router.c` to properly reimplement, in userspace, everything the kernel would normally handle for us. It answers ARP requests for our two router IPs, learns IP to MAC mappings into a small per port ARP cache, looks up the destination IP against a static routing table to decide which port to send a packet out of, decrements the IP time to live and recomputes the header checksum, resolves the next hop MAC address, rewrites the Ethernet header for that hop, and transmits.

## The link local address problem

Once we had the router running, our stats showed zero packets forwarded and zero ARP replies sent, despite ARP requests being received. The router kept logging no route errors for addresses like `169.254.255.255` and `169.254.146.160`.

The 169.254 range is the link local self assigned address a machine gives itself when it fails to get a real IP through DHCP or static configuration. This told us clearly that neither VM had actually picked up the static IPs we intended. Since neither VM believed it was on the 192.168.10.0/24 or 192.168.20.0/24 subnet, neither one ever sent an ARP request targeting our router's addresses, which is exactly why the router had nothing to reply to.

## The real cause, wrong bridged adapter

When we checked the VirtualBox network settings on both VMs, we found the actual root cause. Both VMs had their second network adapter, the one meant to face the Ubuntu box, bridged to the laptop's Wi-Fi adapter instead of its wired Ethernet controller. One VM was bridged to an Intel Wi-Fi 6 AX200 adapter, the other to a Realtek wireless LAN adapter. This meant the VM's DPDK facing traffic was actually leaving over Wi-Fi onto a completely different network, never touching the physical cable running to the Ubuntu box at all, which explains both the link local address and the total silence on the router side.

We fixed this by changing the Name field under the Bridged Adapter setting on each VM to the laptop's actual wired Ethernet controller instead, an Intel Ethernet Connection I219-V on one laptop and a Realtek PCIe GbE Family Controller on the other. After switching this and reassigning the static IPs inside each VM, the physical path from VM to Ubuntu box was finally correct.

## Where we are now

Both laptops are correctly bridged to their wired Ethernet controllers rather than Wi-Fi. Our router code compiles and runs cleanly on the Ubuntu box, pinned to NUMA node 1 cores. The remaining step is confirming the VMs pick up their static IPs correctly on the fixed interfaces and that a ping from VM1 to VM2 produces real ARP replies and forwarded packet logs on the router console.
