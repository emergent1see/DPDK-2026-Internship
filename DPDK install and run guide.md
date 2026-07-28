# DPDK Installation & Execution Guide
### (Ubuntu Server 24.04 guest, VirtualBox host on Windows, ICH9 chipset for VFIO)

This assumes: VM already created, ICH9 chipset selected (Settings → System → Motherboard → Chipset), Ubuntu Server 24.04 installed, two network adapters attached (Adapter 1 = NAT, Adapter 2 = Internal/Host-only for your DPDK port).

---

## 1. System Update & Base Tools

```bash
sudo apt update && sudo apt full-upgrade -y
sudo apt install -y git build-essential curl wget vim net-tools pciutils
sudo reboot
```

---

## 2. Enable IOMMU (now real, via ICH9 emulated VT-d)

Edit GRUB:
```bash
sudo nano /etc/default/grub
```
Set:
```
GRUB_CMDLINE_LINUX_DEFAULT="intel_iommu=on iommu=pt"
```
Apply and reboot:
```bash
sudo update-grub
sudo reboot
```

Verify IOMMU is active:
```bash
dmesg | grep -e DMAR -e IOMMU
# You should see lines confirming IOMMU groups/enabled
find /sys/kernel/iommu_groups/ -type l | wc -l
# Should return a nonzero number of groups
```

**If this returns 0:** ICH9 alone only enables PCIe emulation — it does **not** turn on IOMMU emulation. In VirtualBox 7.0+, IOMMU type is a separate setting that isn't exposed in the GUI and is off by default even with ICH9 selected. You need to set it explicitly via `VBoxManage`.

With the VM fully powered off, run this from your **Windows host** (Command Prompt or PowerShell, from the VirtualBox install directory if it's not on PATH):
```
VBoxManage modifyvm "your-vm-name" --iommu=intel
```
Confirm it took effect:
```
VBoxManage showvminfo "your-vm-name" | findstr /I iommu
```
Then boot the VM and rerun the `dmesg`/`iommu_groups` checks above. If it's still 0 after this, double-check the chipset is actually ICH9 (chipset can only be changed while the VM is powered off) and that nested VT-x/AMD-V is enabled in Settings → System → Acceleration.

---

## 3. Install DPDK Build Dependencies

```bash
sudo apt install -y \
  meson ninja-build \
  python3-pip python3-pyelftools \
  libnuma-dev pkg-config \
  linux-headers-$(uname -r) \
  libssl-dev zlib1g-dev \
  libpcap-dev \
  kmod
```

Check meson/ninja versions (DPDK needs meson >= 0.53, recent DPDK versions want >= 0.57):
```bash
meson --version
ninja --version
```
If your meson is too old:
```bash
pip3 install --user --upgrade meson
export PATH=$HOME/.local/bin:$PATH
echo 'export PATH=$HOME/.local/bin:$PATH' >> ~/.bashrc
```

---

## 4. Configure Hugepages

```bash
grep Hugepagesize /proc/meminfo    # confirm default size, usually 2048 kB

# Reserve 1024 pages of 2MB = 2GB, at runtime
echo 1024 | sudo tee /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages

# Make persistent across reboots
echo "vm.nr_hugepages=1024" | sudo tee -a /etc/sysctl.conf

# Create and mount hugetlbfs
sudo mkdir -p /mnt/huge
grep -q '/mnt/huge' /etc/fstab || echo "nodev /mnt/huge hugetlbfs defaults 0 0" | sudo tee -a /etc/fstab
sudo mount -a

# Verify
grep Huge /proc/meminfo
```
You want `HugePages_Total` to show 1024 and `HugePages_Free` close to that.

---

## 5. Load VFIO Modules

```bash
sudo modprobe vfio
sudo modprobe vfio-pci

# Confirm loaded
lsmod | grep vfio
```

Make them load at boot:
```bash
echo "vfio" | sudo tee -a /etc/modules
echo "vfio-pci" | sudo tee -a /etc/modules
```

Since you have a real emulated IOMMU via ICH9, you do **not** need `vfio.enable_unsafe_noiommu_mode=1`. If you ever see a "No IOMMU" error from VFIO, that means the chipset/IOMMU setup isn't actually active — re-check step 2 before falling back to noiommu mode.

---

## 6. Identify Your NICs

```bash
lspci | grep -i eth
```
You should see two devices — one for the NAT adapter, one for your Internal/Host-only adapter. Note the PCI address of the one you want to hand to DPDK (leave the NAT one alone so you keep SSH/internet access).

```bash
ip a
```
Cross-reference to confirm which interface name (e.g. `enp0s8`) maps to which PCI address — don't bind your management/SSH interface to DPDK or you'll lose your connection.

---

## 7. Clone, Build, and Install DPDK

```bash
cd ~
git clone https://github.com/DPDK/dpdk.git
cd dpdk
git checkout v24.11    # or latest stable tag; `git tag` to list options

meson setup build
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

Confirm install:
```bash
pkg-config --modversion libdpdk
```

---

## 8. Bind the NIC to DPDK

DPDK ships `dpdk-devbind.py` (installed to your PATH after `ninja install`, or in `usertools/` inside the source tree).

```bash
# See current driver bindings
dpdk-devbind.py --status

# Bind your chosen NIC to vfio-pci (use the PCI address from step 6)
sudo dpdk-devbind.py --bind=vfio-pci 0000:00:08.0

# Confirm
dpdk-devbind.py --status
```
It should now show up under "Network devices using DPDK-compatible driver" bound to `vfio-pci`.

If you ever need to give it back to the kernel:
```bash
sudo dpdk-devbind.py --bind=virtio-pci 0000:00:08.0
```
(use whatever the original kernel driver was — check `--status` output before you unbind, it tells you)

---

## 9. Run testpmd (Sanity Check)

```bash
sudo dpdk-testpmd -l 0-3 -n 4 -- -i
```
Flags:
- `-l 0-3` — lcores 0 through 3
- `-n 4` — number of memory channels (4 is a safe default for a VM)
- `-i` — interactive mode

Inside the testpmd prompt:
```
testpmd> show port info all
testpmd> start
testpmd> show port stats all
```
`Ctrl+C` or type `quit` to exit.

If ports show up with correct MAC addresses and link status, your environment is fully functional end-to-end: hugepages, VFIO, IOMMU groups, and the virtio PMD are all working together correctly.

---

## 10. Running Other Sample Apps

DPDK's example apps live in `~/dpdk/examples/`. They build automatically as part of `ninja -C build` if enabled, or you can build a specific one:

### l2fwd (Layer 2 forwarding between two ports)
```bash
cd ~/dpdk/examples/l2fwd
# already built as part of the main build; binary is at:
sudo ~/dpdk/build/examples/dpdk-l2fwd -l 0-1 -n 4 -- -p 0x3
```
`-p 0x3` is a port mask — binary `11` means ports 0 and 1 are both active, and l2fwd will forward traffic between them.

### helloworld (minimal EAL init test — good for understanding EAL bring-up)
```bash
sudo ~/dpdk/build/examples/dpdk-helloworld -l 0-3
```
This just initializes EAL and prints "hello from core X" per lcore — useful as your first "did EAL actually initialize correctly" check before touching mbuf/mempool code.

### skeleton (minimal single-port RX/TX loop — good template for your own app)
```bash
sudo ~/dpdk/build/examples/dpdk-skeleton -l 0-1 -n 4
```
This is the standard starting point if you're about to write your own DPDK app — it shows the minimal EAL init → mbuf pool creation → port config → RX/TX loop structure in ~150 lines.

---

## 11. Common Issues

| Symptom | Likely cause |
|---|---|
| `EAL: No available hugepages reported` | Step 4 wasn't applied, or was reset by reboot — rerun the `echo 1024 \| tee` command |
| `EAL: Cannot open /dev/vfio/vfio: No such file or directory` | `vfio` module not loaded, or chipset isn't ICH9 — recheck steps 2 and 5 |
| `EAL: rte_eal_pci_probe(): Requested device 0000:00:08.0 cannot be used` | NIC still bound to kernel driver — rerun `dpdk-devbind.py --bind=vfio-pci` |
| `Permission denied` on `/dev/vfio/*` | You forgot `sudo`, or your user isn't in a group with access — running as root/sudo is simplest for dev |
| testpmd shows 0 ports | PCI address bound to vfio-pci doesn't match an actual NIC — double check with `lspci -nn \| grep Eth` |
| VM freezes/crashes on EAL init | Not enough RAM allocated for hugepages relative to total VM RAM — reduce hugepage count or increase VM RAM |

---

## What you now have
A fully working DPDK dev loop: build → bind NIC → run sample app → inspect stats. From here, the natural next step is writing your own app starting from `skeleton` — EAL init, `rte_mempool` creation, `rte_eth_rx_burst`/`tx_burst` loop — which maps directly onto the internals you already studied (PMD, rte_mbuf, rte_ring).

Happy to help scaffold that next app whenever you're ready.
