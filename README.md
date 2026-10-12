# Project 2

- Name: Harrison Jansen van Beek
- Email: harrisonjansenva@u.boisestate.edu
- Class: CS425-001

## Known Bugs or Issues

No known bugs or issues.

## Experience

I heavily use AI to build this project, as was encouraged in the class. Overall, I have a good understanding of ARQ as a concept and felt like the code my agents wrote was logical. I probably should dig more into the code written, but with OS and architecture currently breathing down my neck, I relied more heavily on Claude than I normally would. 

## Design

The program is split into three layers so that all of the protocol logic can be unit tested
without a socket, a clock, or a file. A lossy relay is random and slow, but a test that controls
every packet and every "current time" is deterministic.

1. **Packets** (`src/packet.c`). Pure functions: `pkt_checksum` (RFC 1071), `pkt_encode`, and
   `pkt_decode`, which validates a datagram (minimum size, length matches size, length at most
   1024, known type, reserved is 0, checksum). Fields are written byte by byte in network order,
   never by copying a struct onto the wire, and the length is checked against the datagram size
   before any payload byte is copied.
2. **Go-Back-N state machines** (`src/gbn_sender.c`, `src/gbn_receiver.c`). The sender is a struct
   plus `gbn_sender_init`, `gbn_sender_on_ack` and `gbn_sender_on_timeout`. The receiver is a
   struct plus `gbn_receiver_on_packet`. The current time is always passed in as a parameter in
   milliseconds, and each call returns an action describing what to do: which packets to send,
   what payload to deliver, whether to send an ACK, and when the timer is due. Nothing here calls
   `sendto`, `recvfrom`, `poll`, `clock_gettime`, or `fwrite`, and nothing allocates. The sender
   keeps the already-encoded bytes of every in-flight packet in a fixed 64-slot ring, so a timeout
   resends exactly what was sent before. The FIN is treated as one more packet in the same ring.
3. **I/O** (`src/main.c`). The only file that knows about sockets, `poll`, `CLOCK_MONOTONIC`, and
   files. It resolves the relay with `getaddrinfo`, opens one connected UDP socket, performs the
   relay hello (up to five attempts, one second apart), and then runs a thin loop: wait for a
   datagram or the timer, hand the event to the state machine, and carry out the returned action.
   The receiver's 30 second idle timeout and 2 second linger live here, as does the mapping to
   exit codes 0, 1 and 2.

Command line parsing (`src/cli.c`) is a fourth, pure piece: it only turns `argv` into a
configuration struct and validates ranges, so it is tested without running the program.

Because the state machines only see events and a fake clock, the tests can lose, corrupt or
duplicate any packet by simply not passing it along, and can trigger a timeout by passing a
larger "now". `tests/lab-test.c` uses this to run a complete transfer between the real sender and
receiver state machines through an in-memory channel with 20% loss, corruption and duplication in
each direction, using a fixed-seed random number generator. The same split is why `main.c` is
the only file excluded from the coverage report: everything else is reachable from the tests.

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
