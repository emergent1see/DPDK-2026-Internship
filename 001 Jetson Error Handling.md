For this to work, Laptop 1 needs the correct static IP and gateway on the new scheme, plus a specific route to the Jetson's subnet, since this router has no default route logic at all, every subnet crossing needs its own explicit route.

## Confirm Laptop 1's own config first

```
ipconfig
```
Confirm the Ethernet adapter facing the router shows `172.20.10.10` with gateway `172.20.10.1`. If it's still showing the old `192.168.10.10` address from before we moved off that colliding subnet, fix that first:
```
netsh interface ip set address name="Ethernet" static 172.20.10.10 255.255.255.0 172.20.10.1
```
Adjust the interface name if yours differs, check the exact name from `ipconfig`.

## Add the route to reach the Jetson's subnet

```
route -p add 192.168.30.0 mask 255.255.255.0 172.20.10.1
```

## Confirm the router itself is running with the corrected addressing

On the Ubuntu box, confirm the banner shows `172.20.10.1` for port 0 and `192.168.30.1` for port 1, if you rebuilt and restarted it after the IP scheme change.

## Then ping the Jetson directly

```
ping 192.168.30.10
```

Watch the router console at the same time, you should see:
```
[ROUTE] ICMP: 172.20.10.10 -> 192.168.30.10 | in=port0 out=port1
```

If this comes back with no reply, the most likely reason right now is the Jetson itself doesn't have an ARP entry for `172.20.10.1` yet or its route back toward Laptop 1's subnet isn't set, confirm on the Jetson:
```bash
ip route add 172.20.10.0/24 via 192.168.30.1
```
since replies need a path back just as much as the request needs a path there.

Run `ipconfig` on Laptop 1 first and tell me what it currently shows, that confirms whether you're already on the corrected scheme or still need the address fixed before anything else will work.
