# Project 2

- Name: Harrison Jansen van Beek
- Email: harrisonjansenva@u.boisestate.edu
- Class: CS425-001

## Known Bugs or Issues

No known bugs or issues.

## Experience

I heavily use AI to build this project, as was encouraged in the class. Overall, I have a good understanding of ARQ as a concept and felt like the code my agents wrote was logical. I probably should dig more into the code written, but with OS and architecture currently breathing down my neck, I relied more heavily on Claude than I normally would. 

## Results

Setup: 1 MiB random file (`head -c 1048576 /dev/urandom`), relay started with `--delay 50` (100 ms
round trip), default 250 ms timeout, three runs per combination, every output verified with `cmp`
(all 12 identical, all exit codes 0). Times are wall-clock for the sender process, measured on a
single machine over loopback. Throughput is 1024 KiB divided by the mean time.

| Window | Loss | Run 1 (s) | Run 2 (s) | Run 3 (s) | Mean (s) | Throughput (KiB/s) |
| -----: | ---: | --------: | --------: | --------: | -------: | -----------------: |
|      1 |    0 |    105.74 |    105.73 |    105.58 |   105.68 |                9.7 |
|     16 |    0 |      6.80 |      6.79 |      6.79 |     6.79 |              150.8 |
|      1 | 0.05 |    134.63 |    133.88 |    134.31 |   134.27 |                7.6 |
|     16 | 0.05 |     18.74 |     28.35 |     19.85 |    22.31 |               45.9 |

### Analysis

**Round trip time.** With window 1 and no loss the sender sent 1025 packets and waited for each
ACK, so the mean round trip it saw was 105.68 s / 1025 ≈ 103.1 ms. The relay accounts for 100 ms
(50 ms each way). The remaining ~3 ms per packet is overhead outside the relay's delay: the relay
is a Python process that reads, schedules and forwards each datagram, so it adds processing and
timer granularity in both directions; our own `poll` wakeup, packet encoding, checksum, and the
`sendto`/`recv` system calls, plus loopback and scheduler latency, add the rest.

**Speedup at window 16.** The 16-packet window took 6.79 s versus 105.68 s, a speedup of about
15.6x, very close to the ideal 16x. With 16 packets in flight, the sender needs about
1025 / 16 ≈ 64 round trips, and 64 × 103 ms ≈ 6.6 s, which matches the measurement. It is not
exactly 16x because the last partial window and the FIN still cost a whole round trip, and the
same ~3 ms of per-round-trip overhead remains. Since the relay imposes no bandwidth limit, the window is the only thing limiting throughput, so
scaling is near linear.

**Why 5% loss hurts window 16 far more.** At window 1 loss added 28.6 s (+27%); at window 16 it
added about 15.5 s (+229%, 3.3x slower). Each loss costs a full 250 ms timeout in both cases,
but what the timeout resends differs. With window 1, only the single lost packet is outstanding,
so the timeout resends exactly one packet and costs about one extra timeout of waiting (the
relay's counters show 1132 datagrams sent for the 1025 needed, about 10% overhead: loss hits
either the DATA or its ACK, and either one costs one retransmission). With window 16, a loss
anywhere in the window stalls everything behind it: the receiver discards every later packet
(it does not buffer out-of-order data) and keeps re-ACKing the same number, so `base` cannot
move until the timer expires, and then the sender goes back and resends up to 16 packets, most
of which the receiver had already received once. That wasted work is also exposed to the 5% loss
itself, which can trigger further timeouts. The relay showed 1658 datagrams sent to the receiver
for the same file (about 62% more than the 1025 needed), compared with 1132 at window 1.
Because the window-16 baseline is so fast (6.8 s), each stalled 250 ms plus the go-back resend
is a large fraction of the total. The window-16 loss runs also varied more (18.7 s to 28.4 s)
since a handful of extra consecutive timeouts changes the total noticeably.
