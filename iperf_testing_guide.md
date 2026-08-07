# iperf3 Testing Guide for the DPDK Router Lab

This guide covers installing and using `iperf3` to measure real throughput across the
DPDK Layer-3 router, across the same three-VM topology used throughout this project. It
complements the functional tests already documented (ping, `nc`, the custom client/server),
by adding actual bandwidth, jitter, and packet-loss numbers instead of just "it works."

## Table of Contents

- [Why iperf3](#why-iperf3)
- [Topology Recap](#topology-recap)
- [Installation](#installation)
- [Basic TCP Test](#basic-tcp-test)
- [UDP Test](#udp-test)
- [Reverse Mode](#reverse-mode)
- [Bidirectional Test](#bidirectional-test)
- [Parallel Streams](#parallel-streams)
- [Reading the Output](#reading-the-output)
- [Correlating With the Router's Own Logs](#correlating-with-the-routers-own-logs)
- [Suggested Test Matrix](#suggested-test-matrix)
- [Logging Results for the Repo](#logging-results-for-the-repo)
- [Troubleshooting](#troubleshooting)

## Why iperf3

Everything tested so far (ping, `nc`, the Hi/Hello client-server) proves the router
forwards traffic *correctly*. None of it tells you *how much* traffic it can move, or
what happens under sustained load. `iperf3` fills that gap:

- **TCP mode** measures achievable throughput with real congestion control, retransmits,
  and window scaling, the same way a real application's traffic would behave.
- **UDP mode** measures raw throughput at a fixed target rate, and directly reports
  packet loss and jitter, since UDP has no retransmission to hide drops the way TCP does.
- Sustained load is what actually exercises the router's fixed-size resources
  (`RX_RING_SIZE`/`TX_RING_SIZE` = 128, the 16-slot ARP cache), a single ping or a typed
  `nc` message never will.

## Topology Recap

```
VM2 (192.168.100.2/24)  <---->  [ DPDK Router ]  <---->  VM1 (192.168.200.2/24)
                          Port0             Port1
```

`iperf3` runs as an ordinary userspace application on **VM1 and VM2 only**, over their
normal kernel network stacks. It is never installed on the router VM itself, the router
has no IP stack above L3 and no socket API to run `iperf3` against, it only forwards
packets between the two endpoints. All traffic between VM1 and VM2 necessarily passes
through the router, so any throughput measured is genuinely a measurement of the router's
forwarding path.

## Installation

On both VM1 and VM2:

```bash
sudo apt update
sudo apt install -y iperf3
```

Verify:

```bash
iperf3 --version
```

## Basic TCP Test

**On VM1 (server):**

```bash
iperf3 -s
```

Leaves it listening on the default port (5201) until you stop it with `Ctrl+C`, or add
`-1` to have it exit after one test:

```bash
iperf3 -s -1
```

**On VM2 (client):**

```bash
iperf3 -c 192.168.200.2 -t 30
```

`-t 30` runs the test for 30 seconds. Default behavior is a single TCP stream, sending
from client to server.

Expected output shape:

```
Connecting to host 192.168.200.2, port 5201
[  5] local 192.168.100.2 port 44212 connected to 192.168.200.2 port 5201
[ ID] Interval           Transfer     Bitrate         Retr  Cwnd
[  5]   0.00-1.00   sec   112 MBytes   941 Mbits/sec    0    365 KBytes
[  5]   1.00-2.00   sec   111 MBytes   933 Mbits/sec    0    365 KBytes
...
[ ID] Interval           Transfer     Bitrate         Retr
[  5]   0.00-30.00  sec  3.25 GBytes   931 Mbits/sec  0             sender
[  5]   0.00-30.00  sec  3.25 GBytes   930 Mbits/sec               receiver
```

## UDP Test

UDP is the more revealing test for this router specifically, since there's no
retransmission masking anything the forwarding path drops.

**On VM1 (server):** same as before, `iperf3 -s`.

**On VM2 (client):**

```bash
iperf3 -c 192.168.200.2 -u -b 500M -t 30
```

- `-u` switches to UDP.
- `-b 500M` sets the target bandwidth (UDP has no congestion control, so you must
  specify a rate, otherwise `iperf3` defaults to a low 1 Mbit/sec baseline that won't
  tell you anything useful).

Expected output:

```
[ ID] Interval           Transfer     Bitrate         Total Datagrams
[  5]   0.00-30.00  sec   1.75 GBytes   500 Mbits/sec  1279613
[ ID] Interval           Transfer     Bitrate         Jitter    Lost/Total Datagrams
[  5]   0.00-30.00  sec   1.75 GBytes   500 Mbits/sec  0.031 ms  142/1279613 (0.011%)
```

The `Lost/Total Datagrams` line is the number that matters most for this router. Any
loss here at a rate well under your NIC's real capacity is a direct signal that
something in the forwarding path, most likely the fixed 128-slot RX/TX rings, is the
bottleneck, not the physical network.

## Reverse Mode

Tests server-to-client instead of client-to-server, useful for checking whether the
router behaves symmetrically in both directions (it should, since `handle_ipv4()`
doesn't distinguish direction, but this is worth confirming rather than assuming):

```bash
iperf3 -c 192.168.200.2 -R -t 30
```

## Bidirectional Test

Runs both directions simultaneously:

```bash
iperf3 -c 192.168.200.2 --bidir -t 30
```

This is the closest single test to real mixed traffic, and the most likely to expose
any asymmetry in ring sizing or per-port processing time in `process_port()`, since
your main loop currently services Port 0 and Port 1 sequentially rather than truly in
parallel:

```c
while (running) {
    process_port(PORT_VM2, mbuf_pool);
    process_port(PORT_VM1, mbuf_pool);
}
```

Under genuinely bidirectional load, this sequential polling means Port 1 always waits
for Port 0's burst to finish processing first, worth watching for in bidirectional
results.

## Parallel Streams

```bash
iperf3 -c 192.168.200.2 -P 4 -t 30
```

`-P 4` opens 4 parallel TCP streams. Since your router's ARP cache only has 16 slots
per port but this test still only involves 2 real hosts, this specifically tests
*per-connection* handling (multiple TCP streams between the same IP pair) rather than
cache capacity, useful for isolating router throughput ceiling from ARP cache limits.

## Reading the Output

| Field | Meaning |
|---|---|
| **Transfer** | Total data moved during the interval |
| **Bitrate** | Throughput for that interval |
| **Retr** (TCP only) | TCP retransmissions, non-zero values suggest packet loss somewhere in the path forcing TCP to resend |
| **Cwnd** (TCP only) | TCP congestion window, how much data TCP has in flight, useful for spotting congestion-control throttling |
| **Jitter** (UDP only) | Variation in packet arrival timing, higher jitter usually means inconsistent processing time per packet |
| **Lost/Total Datagrams** (UDP only) | Direct packet loss count, the most important UDP metric for this router |

A TCP test showing full expected throughput with `Retr` at or near 0 is a strong signal
the router's forwarding path isn't the bottleneck at that load level. Climbing `Retr`
counts as you increase parallel streams or duration is a signal worth investigating
against the router's own RX/TX ring sizes.

## Correlating With the Router's Own Logs

The router already logs every forwarded packet with `[ROUTE] <proto>: ...` lines. At
`iperf3` traffic rates this will scroll far too fast to read directly, but it's useful
to temporarily add a periodic counter instead of a per-packet log line when running
these tests, something like a running total of packets forwarded per port, printed once
a second, so you can watch the router's own view of throughput next to `iperf3`'s client
and server-side numbers. If all three roughly agree, you've confirmed the whole path
end to end. If the router's counted throughput is meaningfully lower than what `iperf3`
reports as sent, that gap is packets being dropped somewhere in `handle_ipv4()`, most
likely the ARP-miss-drop path if the cache is thrashing, or the ring buffers if the
burst rate is exceeding `BURST_SIZE`/`RX_RING_SIZE`.

## Suggested Test Matrix

A reasonable sequence to actually characterize this router's behavior, worth running in
this order and recording the results:

| # | Test | Command | What it tells you |
|---|---|---|---|
| 1 | Baseline TCP | `iperf3 -c <ip> -t 30` | Sustainable throughput with real congestion control |
| 2 | Baseline UDP | `iperf3 -c <ip> -u -b 500M -t 30` | Raw forwarding capacity, direct loss visibility |
| 3 | UDP ramp | Repeat at `-b 100M`, `250M`, `500M`, `750M`, `1000M` | Find the bandwidth where loss starts appearing, the router's real ceiling |
| 4 | Reverse | `iperf3 -c <ip> -R -t 30` | Confirms symmetric behavior in both directions |
| 5 | Bidirectional | `iperf3 -c <ip> --bidir -t 30` | Exposes any asymmetry from sequential port polling |
| 6 | Parallel streams | `iperf3 -c <ip> -P 4 -t 30` | Per-connection scaling, separate from ARP cache capacity |
| 7 | Long duration | `iperf3 -c <ip> -t 300` | Checks for any slow degradation over time (cache thrash, memory pressure) |

Step 3 is the most valuable single test for this specific router: since `RX_RING_SIZE`
and `TX_RING_SIZE` are both set to the fairly small value of 128, there's a good chance
you'll find a clear point where UDP loss starts climbing well below what the underlying
virtual NIC could otherwise carry, that's your router's actual current throughput
ceiling, and a direct, measured argument for increasing the ring sizes as a next step.

## Logging Results for the Repo

`iperf3` supports JSON output, useful for keeping a clean, parseable record instead of
copy-pasting terminal text into documentation by hand:

```bash
iperf3 -c 192.168.200.2 -u -b 500M -t 30 -J > results/udp-500M-$(date +%Y%m%d-%H%M).json
```

Worth committing a `results/` directory to the repo with a short table summarizing key
runs (date, test type, bandwidth requested, bandwidth achieved, loss percentage), this
turns your testing into an actual longitudinal record you can point to when you improve
the ring sizes or ARP cache later and want to show the before/after difference.

## Troubleshooting

**Client connects but immediately reports 0 throughput, or hangs on connect.**
Check the ARP cache is actually populated first, run a single `ping` between the two
IPs before starting `iperf3`, so you're not accidentally measuring the one-time
ARP-miss-drop delay as if it were sustained packet loss.

**UDP test shows very high loss even at low bandwidth (well under 100 Mbit/sec).**
This points at something more fundamental than ring sizing, worth checking VirtualBox's
adapter type and confirming Promiscuous Mode is still set to "Allow All" on both
DPDK-facing adapters, per the earlier troubleshooting notes in the main router doc.

**TCP throughput caps out far below what you'd expect from the virtual NIC.**
Check `Cwnd` in the output, if it's small and not growing, that's TCP's own congestion
control responding to perceived loss or a small buffer somewhere in the path, not
necessarily a router-side limitation. Cross-check against a UDP test at the same rate to
separate "TCP congestion control being conservative" from "the router is actually
dropping packets."

**Results are inconsistent between runs.**
Run each test multiple times, and note whether you're testing right after a reboot
(cold ARP cache, first packet always drops) versus mid-session with an already-populated
cache, that alone can shift your first-second numbers noticeably on short test durations.
