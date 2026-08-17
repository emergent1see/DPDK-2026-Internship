Let's first see exactly what's sitting there before deleting anything, since your Laptop 1 routing table has picked up several leftover entries across today's changes, the old `192.168.20.0/24` peer route, possibly the `192.168.137.0/24` one from the ICS test, and potentially an old default route through `192.168.10.1` before we moved to `172.20.10.1`.

On Laptop 1:
```
route print
```

Paste the output, then I'll give you the exact `route delete` commands for whichever stale entries are actually there rather than guessing and possibly deleting something still in use.

If you already know exactly which one you mean, here are the likely candidates directly:

```
route delete 192.168.20.0
route delete 192.168.137.0
route delete 0.0.0.0 mask 0.0.0.0 192.168.10.1
```

Run whichever of these matches what's actually leftover, `route print` first will confirm which ones genuinely still exist versus already gone.
