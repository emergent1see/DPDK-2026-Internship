On the Ubuntu box, first check the kernel driver each device would return to:

```bash
sudo dpdk-devbind.py --status
```

Look at the DPDK-compatible driver section for both `0000:86:00.0` and `0000:d8:00.0`, it typically shows something like `unused=ixgbe` or `unused=i40e` next to each, note whatever driver name appears there.

Unbind both, letting them fall back to that kernel driver:

```bash
sudo dpdk-devbind.py -u 0000:86:00.0
sudo dpdk-devbind.py -u 0000:d8:00.0
```

Confirm they moved back to kernel control:

```bash
sudo dpdk-devbind.py --status
ip link show
```

Both `enp134s0f0` and `enp216s0f0` should reappear in `ip link show` once unbound, giving you the normal kernel tools to actually check cable and link status directly:

```bash
ip link show enp134s0f0
ip link show enp216s0f0
```

Look for `state UP` with a carrier versus `state DOWN` or `NO-CARRIER`, that tells you definitively whether each cable is actually connected and the far end is live, something you can't check at all while these ports are DPDK bound, since they have no kernel visibility in that state.

Once you've confirmed the cable situation and are ready to go back to testing, rebind with:
```bash
sudo modprobe vfio-pci
sudo dpdk-devbind.py --bind=vfio-pci 0000:86:00.0
sudo dpdk-devbind.py --bind=vfio-pci 0000:d8:00.0
```
