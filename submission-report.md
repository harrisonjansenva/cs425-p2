# Submission Report

- Submission generated at 10/12/2026 at 02:27:36

- Machine info: Linux runnervmmprz5 6.17.0-1022-azure #22-Ubuntu SMP Mon Jul 27 17:24:03 UTC 2026 x86_64 x86_64 x86_64 GNU/Linux

## Note to Students

Please read this report carefully before submission.
Ensure that all sections are complete and accurate.
Look for any errors in the build or test outputs.
If you find any issues, correct them before submitting.
Post any questions on the class discussion board for help.


---

## README

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

---


## Build Output

This section was generated by running `make all` in the project root directory.

```bash
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/packet.c -o build/debug/packet.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/cli.c -o build/debug/cli.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/gbn_receiver.c -o build/debug/gbn_receiver.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/gbn_sender.c -o build/debug/gbn_sender.c.o
mkdir -p build/debug
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug/main.c.o
cc -g -O0 -DDEBUG -fno-omit-frame-pointer -fsanitize=address build/debug/packet.c.o build/debug/cli.c.o build/debug/gbn_receiver.c.o build/debug/gbn_sender.c.o build/debug/main.c.o -o build/debug/myapp_d -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/packet.c -o build/release/packet.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/cli.c -o build/release/cli.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/gbn_receiver.c -o build/release/gbn_receiver.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/gbn_sender.c -o build/release/gbn_sender.c.o
mkdir -p build/release
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion -c src/main.c -o build/release/main.c.o
cc -Wall -Wextra -O2 -fPIE -MMD -MP -Wformat -Wformat=2 -Wconversion -Wsign-conversion -Wimplicit-fallthrough -fstack-protector-strong -Werror=format-security -Werror=implicit -Werror=incompatible-pointer-types -Werror=int-conversion build/release/packet.c.o build/release/cli.c.o build/release/gbn_receiver.c.o build/release/gbn_sender.c.o build/release/main.c.o -o build/release/myapp 
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/packet.c -o build/tests/packet.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/cli.c -o build/tests/cli.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/gbn_receiver.c -o build/tests/gbn_receiver.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/gbn_sender.c -o build/tests/gbn_sender.c.o
mkdir -p build/tests
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c src/main.c -o build/tests/main.c.o
mkdir -p build/tests/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/lab-test.c -o build/tests/lab-test.c.o
mkdir -p build/tests/harness/
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage -c tests/harness/unity.c -o build/tests/harness/unity.c.o
cc -g -O0 -DTEST -fprofile-arcs -ftest-coverage build/tests/packet.c.o build/tests/cli.c.o build/tests/gbn_receiver.c.o build/tests/gbn_sender.c.o build/tests/main.c.o build/tests/lab-test.c.o build/tests/harness/unity.c.o -o build/tests/myapp_t -fprofile-arcs -ftest-coverage
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
make[1]: Entering directory '/home/runner/work/cs425-p2/cs425-p2'
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/packet.c -o build/debug-test/packet.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/cli.c -o build/debug-test/cli.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/gbn_receiver.c -o build/debug-test/gbn_receiver.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/gbn_sender.c -o build/debug-test/gbn_sender.c.o
mkdir -p build/debug-test
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c src/main.c -o build/debug-test/main.c.o
mkdir -p build/debug-test/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/lab-test.c -o build/debug-test/lab-test.c.o
mkdir -p build/debug-test/harness/
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address -c tests/harness/unity.c -o build/debug-test/harness/unity.c.o
cc -g -O0 -DDEBUG -DTEST -fno-omit-frame-pointer -fsanitize=address build/debug-test/packet.c.o build/debug-test/cli.c.o build/debug-test/gbn_receiver.c.o build/debug-test/gbn_sender.c.o build/debug-test/main.c.o build/debug-test/lab-test.c.o build/debug-test/harness/unity.c.o -o build/debug-test/myapp_td -fsanitize=address
make[1]: Leaving directory '/home/runner/work/cs425-p2/cs425-p2'
Builds completed. You can run the application with: ./build/release/myapp
You can run the debug build with: ./build/debug/myapp_d
You can run the test build with: ./build/tests/myapp_t
You can run the debug-test build with: ./build/debug-test/myapp_td
```

---

## Coverage Report

This section was generated by running `make report` in the project root directory.

```bash
tests/lab-test.c:972:test_checksum_rfc1071_example:PASS
tests/lab-test.c:973:test_encode_hi_example_matches_literal:PASS
tests/lab-test.c:974:test_decode_hi_example_round_trips:PASS
tests/lab-test.c:975:test_decode_hi_example_single_bit_flip_fails_checksum:PASS
tests/lab-test.c:976:test_decode_ack3_literal:PASS
tests/lab-test.c:977:test_encode_ack3_matches_literal:PASS
tests/lab-test.c:978:test_decode_too_short:PASS
tests/lab-test.c:979:test_decode_length_mismatch:PASS
tests/lab-test.c:980:test_decode_payload_too_long:PASS
tests/lab-test.c:981:test_decode_bad_type:PASS
tests/lab-test.c:982:test_decode_bad_reserved:PASS
tests/lab-test.c:983:test_decode_bad_checksum:PASS
tests/lab-test.c:984:test_encode_payload_too_long_returns_zero:PASS
tests/lab-test.c:986:test_receiver_in_order_delivery:PASS
tests/lab-test.c:987:test_receiver_duplicate_data_is_discarded:PASS
tests/lab-test.c:988:test_receiver_data_past_a_gap_is_discarded:PASS
tests/lab-test.c:989:test_receiver_repeated_fin_after_finished:PASS
tests/lab-test.c:990:test_receiver_data_during_linger_is_discarded:PASS
tests/lab-test.c:991:test_receiver_stray_ack_is_noop:PASS
tests/lab-test.c:993:test_sender_small_file_single_burst:PASS
tests/lab-test.c:994:test_sender_window_full_sends_exactly_window_packets:PASS
tests/lab-test.c:995:test_sender_cumulative_ack_slides_and_sends_more:PASS
tests/lab-test.c:996:test_sender_stale_ack_is_noop:PASS
tests/lab-test.c:997:test_sender_timeout_resend_is_byte_identical:PASS
tests/lab-test.c:998:test_sender_ten_consecutive_timeouts_fails:PASS
tests/lab-test.c:999:test_sender_progress_resets_timeout_counter:PASS
tests/lab-test.c:1000:test_sender_empty_file_sends_lone_fin:PASS
tests/lab-test.c:1001:test_sender_exact_multiple_of_1024_has_no_trailing_short_packet:PASS
tests/lab-test.c:1003:test_cli_no_args_is_usage:PASS
tests/lab-test.c:1004:test_cli_valid_send_all_flags:PASS
tests/lab-test.c:1005:test_cli_valid_send_minimal_uses_defaults:PASS
tests/lab-test.c:1006:test_cli_valid_recv:PASS
tests/lab-test.c:1007:test_cli_missing_session_is_error:PASS
tests/lab-test.c:1008:test_cli_window_out_of_range_is_error:PASS
tests/lab-test.c:1009:test_cli_rate_out_of_range_is_error:PASS
tests/lab-test.c:1010:test_cli_wrong_positional_count_is_error:PASS
tests/lab-test.c:1011:test_cli_invalid_session_chars_is_error:PASS
tests/lab-test.c:1012:test_cli_unknown_mode_is_error:PASS
tests/lab-test.c:1013:test_cli_send_only_flag_on_recv_is_error:PASS
tests/lab-test.c:1014:test_cli_session_too_long_is_error:PASS
tests/lab-test.c:1015:test_cli_malformed_numbers_are_errors:PASS
tests/lab-test.c:1016:test_cli_empty_flag_value_is_error:PASS
tests/lab-test.c:1017:test_cli_each_flags_own_parse_failure_is_error:PASS
tests/lab-test.c:1018:test_cli_print_usage_does_not_crash:PASS
tests/lab-test.c:1019:test_cli_repeated_calls_reset_getopt_state:PASS
tests/lab-test.c:1021:test_e2e_seeded_lossy_transfer:PASS

-----------------------
46 Tests 0 Failures 0 Ignored 
OK
./build/tests/myapp_t
tests/lab-test.c:972:test_checksum_rfc1071_example:PASS
tests/lab-test.c:973:test_encode_hi_example_matches_literal:PASS
tests/lab-test.c:974:test_decode_hi_example_round_trips:PASS
tests/lab-test.c:975:test_decode_hi_example_single_bit_flip_fails_checksum:PASS
tests/lab-test.c:976:test_decode_ack3_literal:PASS
tests/lab-test.c:977:test_encode_ack3_matches_literal:PASS
tests/lab-test.c:978:test_decode_too_short:PASS
tests/lab-test.c:979:test_decode_length_mismatch:PASS
tests/lab-test.c:980:test_decode_payload_too_long:PASS
tests/lab-test.c:981:test_decode_bad_type:PASS
tests/lab-test.c:982:test_decode_bad_reserved:PASS
tests/lab-test.c:983:test_decode_bad_checksum:PASS
tests/lab-test.c:984:test_encode_payload_too_long_returns_zero:PASS
tests/lab-test.c:986:test_receiver_in_order_delivery:PASS
tests/lab-test.c:987:test_receiver_duplicate_data_is_discarded:PASS
tests/lab-test.c:988:test_receiver_data_past_a_gap_is_discarded:PASS
tests/lab-test.c:989:test_receiver_repeated_fin_after_finished:PASS
tests/lab-test.c:990:test_receiver_data_during_linger_is_discarded:PASS
tests/lab-test.c:991:test_receiver_stray_ack_is_noop:PASS
tests/lab-test.c:993:test_sender_small_file_single_burst:PASS
tests/lab-test.c:994:test_sender_window_full_sends_exactly_window_packets:PASS
tests/lab-test.c:995:test_sender_cumulative_ack_slides_and_sends_more:PASS
tests/lab-test.c:996:test_sender_stale_ack_is_noop:PASS
tests/lab-test.c:997:test_sender_timeout_resend_is_byte_identical:PASS
tests/lab-test.c:998:test_sender_ten_consecutive_timeouts_fails:PASS
tests/lab-test.c:999:test_sender_progress_resets_timeout_counter:PASS
tests/lab-test.c:1000:test_sender_empty_file_sends_lone_fin:PASS
tests/lab-test.c:1001:test_sender_exact_multiple_of_1024_has_no_trailing_short_packet:PASS
tests/lab-test.c:1003:test_cli_no_args_is_usage:PASS
tests/lab-test.c:1004:test_cli_valid_send_all_flags:PASS
tests/lab-test.c:1005:test_cli_valid_send_minimal_uses_defaults:PASS
tests/lab-test.c:1006:test_cli_valid_recv:PASS
tests/lab-test.c:1007:test_cli_missing_session_is_error:PASS
tests/lab-test.c:1008:test_cli_window_out_of_range_is_error:PASS
tests/lab-test.c:1009:test_cli_rate_out_of_range_is_error:PASS
tests/lab-test.c:1010:test_cli_wrong_positional_count_is_error:PASS
tests/lab-test.c:1011:test_cli_invalid_session_chars_is_error:PASS
tests/lab-test.c:1012:test_cli_unknown_mode_is_error:PASS
tests/lab-test.c:1013:test_cli_send_only_flag_on_recv_is_error:PASS
tests/lab-test.c:1014:test_cli_session_too_long_is_error:PASS
tests/lab-test.c:1015:test_cli_malformed_numbers_are_errors:PASS
tests/lab-test.c:1016:test_cli_empty_flag_value_is_error:PASS
tests/lab-test.c:1017:test_cli_each_flags_own_parse_failure_is_error:PASS
tests/lab-test.c:1018:test_cli_print_usage_does_not_crash:PASS
tests/lab-test.c:1019:test_cli_repeated_calls_reset_getopt_state:PASS
tests/lab-test.c:1021:test_e2e_seeded_lossy_transfer:PASS

-----------------------
46 Tests 0 Failures 0 Ignored 
OK
mkdir -p ./build/report/html
mkdir -p ./build/report/txt
gcovr -r . --html --html-details --exclude-directories build/tests/harness --exclude '.*main\.c$' --exclude '.*test\.c$' -o ./build/report/html/coverage_report.html
(INFO) Reading coverage data...

(INFO) Writing coverage report...

gcovr -r . --txt                 --exclude-directories build/tests/harness --exclude '.*main\.c$' --exclude '.*test\.c$'
(INFO) Reading coverage data...

(INFO) Writing coverage report...

------------------------------------------------------------------------------
                           GCC Code Coverage Report
Directory: .
------------------------------------------------------------------------------
File                                       Lines     Exec  Cover   Missing
------------------------------------------------------------------------------
src/cli.c                                    117      117   100%
src/gbn_receiver.c                            31       31   100%
src/gbn_sender.c                              74       74   100%
src/packet.c                                  49       49   100%
------------------------------------------------------------------------------
TOTAL                                        271      271   100%
------------------------------------------------------------------------------
```

---

## Address Sanitizer Report

This section was generated by running `make leak-test` in the project root directory.

```bash
tests/lab-test.c:972:test_checksum_rfc1071_example:PASS
tests/lab-test.c:973:test_encode_hi_example_matches_literal:PASS
tests/lab-test.c:974:test_decode_hi_example_round_trips:PASS
tests/lab-test.c:975:test_decode_hi_example_single_bit_flip_fails_checksum:PASS
tests/lab-test.c:976:test_decode_ack3_literal:PASS
tests/lab-test.c:977:test_encode_ack3_matches_literal:PASS
tests/lab-test.c:978:test_decode_too_short:PASS
tests/lab-test.c:979:test_decode_length_mismatch:PASS
tests/lab-test.c:980:test_decode_payload_too_long:PASS
tests/lab-test.c:981:test_decode_bad_type:PASS
tests/lab-test.c:982:test_decode_bad_reserved:PASS
tests/lab-test.c:983:test_decode_bad_checksum:PASS
tests/lab-test.c:984:test_encode_payload_too_long_returns_zero:PASS
tests/lab-test.c:986:test_receiver_in_order_delivery:PASS
tests/lab-test.c:987:test_receiver_duplicate_data_is_discarded:PASS
tests/lab-test.c:988:test_receiver_data_past_a_gap_is_discarded:PASS
tests/lab-test.c:989:test_receiver_repeated_fin_after_finished:PASS
tests/lab-test.c:990:test_receiver_data_during_linger_is_discarded:PASS
tests/lab-test.c:991:test_receiver_stray_ack_is_noop:PASS
tests/lab-test.c:993:test_sender_small_file_single_burst:PASS
tests/lab-test.c:994:test_sender_window_full_sends_exactly_window_packets:PASS
tests/lab-test.c:995:test_sender_cumulative_ack_slides_and_sends_more:PASS
tests/lab-test.c:996:test_sender_stale_ack_is_noop:PASS
tests/lab-test.c:997:test_sender_timeout_resend_is_byte_identical:PASS
tests/lab-test.c:998:test_sender_ten_consecutive_timeouts_fails:PASS
tests/lab-test.c:999:test_sender_progress_resets_timeout_counter:PASS
tests/lab-test.c:1000:test_sender_empty_file_sends_lone_fin:PASS
tests/lab-test.c:1001:test_sender_exact_multiple_of_1024_has_no_trailing_short_packet:PASS
tests/lab-test.c:1003:test_cli_no_args_is_usage:PASS
tests/lab-test.c:1004:test_cli_valid_send_all_flags:PASS
tests/lab-test.c:1005:test_cli_valid_send_minimal_uses_defaults:PASS
tests/lab-test.c:1006:test_cli_valid_recv:PASS
tests/lab-test.c:1007:test_cli_missing_session_is_error:PASS
tests/lab-test.c:1008:test_cli_window_out_of_range_is_error:PASS
tests/lab-test.c:1009:test_cli_rate_out_of_range_is_error:PASS
tests/lab-test.c:1010:test_cli_wrong_positional_count_is_error:PASS
tests/lab-test.c:1011:test_cli_invalid_session_chars_is_error:PASS
tests/lab-test.c:1012:test_cli_unknown_mode_is_error:PASS
tests/lab-test.c:1013:test_cli_send_only_flag_on_recv_is_error:PASS
tests/lab-test.c:1014:test_cli_session_too_long_is_error:PASS
tests/lab-test.c:1015:test_cli_malformed_numbers_are_errors:PASS
tests/lab-test.c:1016:test_cli_empty_flag_value_is_error:PASS
tests/lab-test.c:1017:test_cli_each_flags_own_parse_failure_is_error:PASS
tests/lab-test.c:1018:test_cli_print_usage_does_not_crash:PASS
tests/lab-test.c:1019:test_cli_repeated_calls_reset_getopt_state:PASS
tests/lab-test.c:1021:test_e2e_seeded_lossy_transfer:PASS

-----------------------
46 Tests 0 Failures 0 Ignored 
OK
```

---

## Src Files
### cli.c

```c

#include "lab.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static bool session_is_valid(const char *s)
{
   size_t len = strlen(s);
   if (len < 1 || len > 32)
   {
      return false;
   }
   for (size_t i = 0; i < len; i++)
   {
      unsigned char c = (unsigned char)s[i];
      if (!(islower(c) || isdigit(c) || c == '-'))
      {
         return false;
      }
   }
   return true;
}

static bool parse_u32(const char *s, uint32_t min, uint32_t max, uint32_t *out)
{
   if (s[0] == '\0')
   {
      return false;
   }
   char *endptr = NULL;
   unsigned long val = strtoul(s, &endptr, 10);
   if (endptr == s || *endptr != '\0')
   {
      return false;
   }
   if (val < min || val > max)
   {
      return false;
   }
   *out = (uint32_t)val;
   return true;
}

static bool parse_double_prob(const char *s, double *out)
{
   if (s[0] == '\0')
   {
      return false;
   }
   char *endptr = NULL;
   double val = strtod(s, &endptr);
   if (endptr == s || *endptr != '\0')
   {
      return false;
   }
   if (!(val >= 0.0 && val <= 0.5))
   {
      return false;
   }
   *out = val;
   return true;
}

cli_status_t cli_parse(int argc, char *const argv[], cli_config_t *out)
{
   optind = 1;
#ifdef optreset
   optreset = 1;
#endif

   if (argc == 1)
   {
      return CLI_USAGE;
   }

   bool is_send;
   if (strcmp(argv[1], "send") == 0)
   {
      is_send = true;
   }
   else if (strcmp(argv[1], "recv") == 0)
   {
      is_send = false;
   }
   else
   {
      return CLI_ERROR;
   }

   bool have_session = false;
   uint32_t window = 8;
   uint32_t timeout_ms = 250;
   double loss = 0.0;
   double corrupt = 0.0;
   double dup = 0.0;
   uint32_t port = 4250;
   char session[33];
   session[0] = '\0';

   const char *optstring = is_send ? "s:w:T:l:c:d:p:" : "s:p:";

   opterr = 0;
   int c;
   while ((c = getopt(argc - 1, argv + 1, optstring)) != -1)
   {
      switch (c)
      {
      case 's':
         if (!session_is_valid(optarg))
         {
            return CLI_ERROR;
         }
         strncpy(session, optarg, sizeof(session) - 1);
         session[sizeof(session) - 1] = '\0';
         have_session = true;
         break;
      case 'w':
         if (!parse_u32(optarg, 1, 64, &window))
         {
            return CLI_ERROR;
         }
         break;
      case 'T':
         if (!parse_u32(optarg, 1, UINT32_MAX, &timeout_ms))
         {
            return CLI_ERROR;
         }
         break;
      case 'l':
         if (!parse_double_prob(optarg, &loss))
         {
            return CLI_ERROR;
         }
         break;
      case 'c':
         if (!parse_double_prob(optarg, &corrupt))
         {
            return CLI_ERROR;
         }
         break;
      case 'd':
         if (!parse_double_prob(optarg, &dup))
         {
            return CLI_ERROR;
         }
         break;
      case 'p':
      {
         uint32_t port_val;
         if (!parse_u32(optarg, 1, 65535, &port_val))
         {
            return CLI_ERROR;
         }
         port = port_val;
         break;
      }
      case '?':
      default:
         return CLI_ERROR;
      }
   }

   if (!have_session)
   {
      return CLI_ERROR;
   }

   int remaining = (argc - 1) - optind;
   if (remaining != 2)
   {
      return CLI_ERROR;
   }

   out->mode = is_send ? CLI_MODE_SEND : CLI_MODE_RECV;
   memcpy(out->session, session, sizeof(out->session));
   out->window = window;
   out->timeout_ms = timeout_ms;
   out->loss = loss;
   out->corrupt = corrupt;
   out->dup = dup;
   out->port = (uint16_t)port;
   out->relay_host = argv[1 + optind];
   out->file_path = argv[1 + optind + 1];

   return CLI_OK;
}

void cli_print_usage(FILE *stream)
{
   fprintf(stream, "Usage: myapp send -s <session> [-w window] [-T timeout-ms] [-l loss]\n");
   fprintf(stream, "                  [-c corrupt] [-d dup] [-p port] <relay> <file>\n");
   fprintf(stream, "       myapp recv -s <session> [-p port] <relay> <file>\n");
   fprintf(stream, "\n");
   fprintf(stream, "  -s <session>     session name shared by the sender and the receiver\n");
   fprintf(stream, "  -w <window>      Go-Back-N window size in packets, 1 to 64 (default: 8)\n");
   fprintf(stream, "  -T <timeout-ms>  retransmission timeout in milliseconds (default: 250)\n");
   fprintf(stream, "  -l <loss>        probability the relay drops a packet (default: 0)\n");
   fprintf(stream, "  -c <corrupt>     probability the relay flips a bit (default: 0)\n");
   fprintf(stream, "  -d <dup>         probability the relay duplicates a packet (default: 0)\n");
   fprintf(stream, "  -p <port>        relay port (default: 4250)\n");
   fprintf(stream, "  <relay>          host name or address of the relay\n");
   fprintf(stream, "  <file>           file to send, or file to write what is received\n");
}

```

### gbn_receiver.c

```c

#include "lab.h"

void gbn_receiver_init(gbn_receiver_t *r)
{
   r->expected = 0;
   r->finished = false;
}

void gbn_receiver_on_packet(gbn_receiver_t *r, const packet_t *pkt, gbn_recv_action_t *out)
{
   out->deliver = false;
   out->payload = NULL;
   out->payload_len = 0;
   out->send_ack = false;
   out->ack_seq = 0;

   switch (pkt->type)
   {
   case PKT_DATA:
      if (pkt->seq == r->expected)
      {
         out->deliver = true;
         out->payload = pkt->payload;
         out->payload_len = pkt->length;
         r->expected++;
      }
      out->send_ack = true;
      out->ack_seq = r->expected;
      break;

   case PKT_FIN:
      if (pkt->seq == r->expected)
      {
         r->finished = true;
         r->expected++;
      }
      out->send_ack = true;
      out->ack_seq = r->expected;
      break;

   case PKT_ACK:
   default:
      break;
   }

   out->finished = r->finished;
}

```

### gbn_sender.c

```c

#include "lab.h"

#include <string.h>

/* The only place that sends a NEW packet. Shared by gbn_sender_init() and
 * gbn_sender_on_ack() so the two never drift apart. */
static void gbn_fill(gbn_sender_t *s, uint64_t now_ms, gbn_action_t *out)
{
   out->count = 0;

   while (s->next < s->base + s->window && s->next < s->total)
   {
      uint32_t seq = s->next;
      packet_t pkt;
      pkt.seq = seq;

      if (seq < s->total - 1u)
      {
         size_t offset = (size_t)seq * 1024u;
         size_t remaining = s->file_len - offset;
         uint16_t len = (uint16_t)(remaining < 1024u ? remaining : 1024u);

         pkt.type = PKT_DATA;
         pkt.length = len;
         memcpy(pkt.payload, s->file_data + offset, len);
      }
      else
      {
         pkt.type = PKT_FIN;
         pkt.length = 0;
      }

      gbn_slot_t *slot = &s->ring[seq % GBN_MAX_WINDOW];
      slot->len = (uint16_t)pkt_encode(&pkt, slot->wire);

      out->ptr[out->count] = slot->wire;
      out->len[out->count] = slot->len;
      out->count += 1u;

      s->next += 1u;
   }

   if (s->base < s->next && !s->timer_running)
   {
      s->timer_running = true;
      s->timer_deadline_ms = now_ms + s->timeout_ms;
   }

   out->timer_running = s->timer_running;
   out->timer_deadline_ms = s->timer_deadline_ms;
   out->done = (s->base == s->total);
   out->failed = s->failed;
}

void gbn_sender_init(gbn_sender_t *s, uint32_t window, uint64_t timeout_ms,
                     const uint8_t *file_data, size_t file_len, uint64_t now_ms, gbn_action_t *out)
{
   memset(s, 0, sizeof(*s));

   s->window = window;
   s->timeout_ms = timeout_ms;
   s->file_data = file_data;
   s->file_len = file_len;
   s->total = (uint32_t)((file_len + 1023u) / 1024u) + 1u;

   gbn_fill(s, now_ms, out);
}

void gbn_sender_on_ack(gbn_sender_t *s, uint32_t ack_seq, uint64_t now_ms, gbn_action_t *out)
{
   /* An ACK beyond next acknowledges a packet never sent; treat it like a duplicate. */
   if (ack_seq > s->base && ack_seq <= s->next)
   {
      s->base = ack_seq;
      s->consecutive_timeouts = 0;

      if (s->base < s->next)
      {
         s->timer_deadline_ms = now_ms + s->timeout_ms;
         s->timer_running = true;
      }
      else
      {
         s->timer_running = false;
      }

      gbn_fill(s, now_ms, out);
   }
   else
   {
      out->count = 0;
      out->timer_running = s->timer_running;
      out->timer_deadline_ms = s->timer_deadline_ms;
      out->done = (s->base == s->total);
      out->failed = s->failed;
   }
}

void gbn_sender_on_timeout(gbn_sender_t *s, uint64_t now_ms, gbn_action_t *out)
{
   s->consecutive_timeouts += 1u;

   if (s->consecutive_timeouts >= 10u)
   {
      s->failed = true;
      s->timer_running = false;

      out->count = 0;
      out->done = false;
      out->failed = true;
      out->timer_running = false;
      return;
   }

   out->count = 0;
   for (uint32_t seq = s->base; seq < s->next; seq++)
   {
      gbn_slot_t *slot = &s->ring[seq % GBN_MAX_WINDOW];

      out->ptr[out->count] = slot->wire;
      out->len[out->count] = slot->len;
      out->count += 1u;
   }

   s->timer_deadline_ms = now_ms + s->timeout_ms;
   s->timer_running = true;

   out->timer_running = s->timer_running;
   out->timer_deadline_ms = s->timer_deadline_ms;
   out->done = false;
   out->failed = false;
}

```

### lab.h

```c

#ifndef LAB_H
#define LAB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* ---- Layer 1: packets (src/packet.c) ---- */

#define PKT_HEADER_LEN 10u
#define PKT_MAX_PAYLOAD 1024u
#define PKT_MAX_LEN (PKT_HEADER_LEN + PKT_MAX_PAYLOAD)

typedef enum
{
   PKT_DATA = 0,
   PKT_ACK = 1,
   PKT_FIN = 2
} pkt_type_t;

typedef struct
{
   pkt_type_t type;
   uint32_t seq;
   uint16_t length;
   uint8_t payload[PKT_MAX_PAYLOAD];
} packet_t;

uint16_t pkt_checksum(const uint8_t *data, size_t len);

size_t pkt_encode(const packet_t *pkt, uint8_t out[PKT_MAX_LEN]);

typedef enum
{
   PKT_OK = 0,
   PKT_ERR_TOO_SHORT,
   PKT_ERR_LENGTH_MISMATCH,
   PKT_ERR_PAYLOAD_TOO_LONG,
   PKT_ERR_BAD_TYPE,
   PKT_ERR_BAD_RESERVED,
   PKT_ERR_BAD_CHECKSUM,
} pkt_status_t;

pkt_status_t pkt_decode(const uint8_t *in, size_t in_len, packet_t *out);

/* ---- Layer 2: GBN sender (src/gbn_sender.c) ---- */

#define GBN_MAX_WINDOW 64u

typedef struct
{
   uint16_t len;
   uint8_t wire[PKT_MAX_LEN];
} gbn_slot_t;

typedef struct
{
   uint32_t base, next;
   uint32_t total; /* FIN's seq + 1 (num_data_packets + 1) */
   uint32_t window;
   uint64_t timeout_ms;
   uint64_t timer_deadline_ms;
   bool timer_running;
   uint32_t consecutive_timeouts;
   bool done;
   bool failed;
   const uint8_t *file_data;
   size_t file_len;
   gbn_slot_t ring[GBN_MAX_WINDOW];
} gbn_sender_t;

typedef struct
{
   const uint8_t *ptr[GBN_MAX_WINDOW];
   uint16_t len[GBN_MAX_WINDOW];
   size_t count;
   bool timer_running;
   uint64_t timer_deadline_ms;
   bool done;
   bool failed;
} gbn_action_t;

void gbn_sender_init(gbn_sender_t *s, uint32_t window, uint64_t timeout_ms,
                     const uint8_t *file_data, size_t file_len, uint64_t now_ms, gbn_action_t *out);
void gbn_sender_on_ack(gbn_sender_t *s, uint32_t ack_seq, uint64_t now_ms, gbn_action_t *out);
void gbn_sender_on_timeout(gbn_sender_t *s, uint64_t now_ms, gbn_action_t *out);

/* ---- Layer 2: GBN receiver (src/gbn_receiver.c) ---- */

typedef struct
{
   uint32_t expected;
   bool finished;
} gbn_receiver_t;

typedef struct
{
   bool deliver;
   const uint8_t *payload;
   uint16_t payload_len;
   bool send_ack;
   uint32_t ack_seq;
   bool finished;
} gbn_recv_action_t;

void gbn_receiver_init(gbn_receiver_t *r);
void gbn_receiver_on_packet(gbn_receiver_t *r, const packet_t *pkt, gbn_recv_action_t *out);

/* ---- CLI (src/cli.c) ---- */

typedef enum
{
   CLI_MODE_SEND,
   CLI_MODE_RECV
} cli_mode_t;

typedef struct
{
   cli_mode_t mode;
   char session[33];
   uint32_t window;
   uint32_t timeout_ms;
   double loss, corrupt, dup;
   uint16_t port;
   const char *relay_host;
   const char *file_path;
} cli_config_t;

typedef enum
{
   CLI_OK = 0,
   CLI_USAGE,
   CLI_ERROR
} cli_status_t;

cli_status_t cli_parse(int argc, char *const argv[], cli_config_t *out);
void cli_print_usage(FILE *stream);

#endif /* LAB_H */

```

### main.c

```c

/* Layer 3: the only file that touches sockets, poll, the clock, and files. */
#include "lab.h"

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define EXIT_OK 0
#define EXIT_BAD_USAGE 1
#define EXIT_FAILURE_NET 2

#define HELLO_ATTEMPTS 5
#define HELLO_WAIT_MS 1000u
#define RECV_IDLE_MS 30000u
#define RECV_LINGER_MS 2000u

static uint64_t now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Milliseconds until deadline, 0 if already past. Compares before subtracting. */
static int ms_until(uint64_t now, uint64_t deadline)
{
   if (now >= deadline)
   {
      return 0;
   }
   uint64_t left = deadline - now;
   return left > 60000u ? 60000 : (int)left;
}

/* A transient error is one we treat exactly like a lost datagram. */
static bool is_transient(int err)
{
   return err == ECONNREFUSED || err == EAGAIN || err == EWOULDBLOCK || err == EINTR ||
          err == ENOBUFS;
}

static int open_relay_socket(const char *host, uint16_t port)
{
   char port_str[8];
   snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

   struct addrinfo hints;
   memset(&hints, 0, sizeof(hints));
   hints.ai_family = AF_UNSPEC;
   hints.ai_socktype = SOCK_DGRAM;

   struct addrinfo *res = NULL;
   int rc = getaddrinfo(host, port_str, &hints, &res);
   if (rc != 0)
   {
      fprintf(stderr, "error: cannot resolve '%s': %s\n", host, gai_strerror(rc));
      return -1;
   }

   int fd = -1;
   for (struct addrinfo *ai = res; ai != NULL; ai = ai->ai_next)
   {
      fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
      if (fd < 0)
      {
         continue;
      }
      if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
      {
         break;
      }
      close(fd);
      fd = -1;
   }
   freeaddrinfo(res);

   if (fd < 0)
   {
      fprintf(stderr, "error: cannot create a socket to %s:%s\n", host, port_str);
   }
   return fd;
}

/* Wait up to timeout_ms for a datagram. Returns bytes read, 0 for "nothing usable this round",
 * or -1 on a fatal socket error. */
static ssize_t wait_datagram(int fd, int timeout_ms, uint8_t *buf, size_t cap)
{
   struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
   int rc = poll(&pfd, 1, timeout_ms);
   if (rc < 0)
   {
      return errno == EINTR ? 0 : -1;
   }
   if (rc == 0)
   {
      return 0;
   }

   ssize_t n = recv(fd, buf, cap, 0);
   if (n < 0)
   {
      return is_transient(errno) ? 0 : -1;
   }
   return n;
}

/* Returns false only on a fatal send error; a lost datagram is not an error. */
static bool send_datagram(int fd, const uint8_t *buf, size_t len)
{
   if (send(fd, buf, len, 0) < 0 && !is_transient(errno))
   {
      perror("error: send");
      return false;
   }
   return true;
}

static bool register_with_relay(int fd, const cli_config_t *cfg)
{
   char hello[128];
   int len;
   if (cfg->mode == CLI_MODE_RECV)
   {
      len = snprintf(hello, sizeof(hello), "HELLO %s recv", cfg->session);
   }
   else
   {
      len = snprintf(hello, sizeof(hello), "HELLO %s send %.6f %.6f %.6f", cfg->session, cfg->loss,
                     cfg->corrupt, cfg->dup);
   }

   for (int attempt = 0; attempt < HELLO_ATTEMPTS; attempt++)
   {
      if (!send_datagram(fd, (const uint8_t *)hello, (size_t)len))
      {
         return false;
      }

      uint64_t deadline = now_ms() + HELLO_WAIT_MS;
      for (;;)
      {
         uint64_t now = now_ms();
         if (now >= deadline)
         {
            break;
         }

         char reply[256];
         ssize_t n =
             wait_datagram(fd, ms_until(now, deadline), (uint8_t *)reply, sizeof(reply) - 1);
         if (n < 0)
         {
            perror("error: recv");
            return false;
         }
         if (n == 0)
         {
            continue;
         }

         reply[n] = '\0';
         if (strcmp(reply, "OK") == 0)
         {
            return true;
         }
         if (strncmp(reply, "ERR", 3) == 0)
         {
            fprintf(stderr, "error: relay refused registration: %s\n", reply);
            return false;
         }
         /* Anything else is not a reply to our hello; keep waiting. */
      }
   }

   fprintf(stderr, "error: no reply from relay after %d attempts\n", HELLO_ATTEMPTS);
   return false;
}

static int run_recv(int fd, const cli_config_t *cfg)
{
   FILE *out = fopen(cfg->file_path, "wb");
   if (out == NULL)
   {
      perror("error: cannot open output file");
      return EXIT_FAILURE_NET;
   }

   gbn_receiver_t rx;
   gbn_receiver_init(&rx);

   int status = EXIT_FAILURE_NET;
   uint64_t deadline = now_ms() + RECV_IDLE_MS;
   uint8_t buf[PKT_MAX_LEN + 1];
   packet_t pkt;

   for (;;)
   {
      uint64_t now = now_ms();
      if (now >= deadline)
      {
         if (rx.finished)
         {
            status = EXIT_OK;
         }
         else
         {
            fprintf(stderr, "error: nothing valid received for %u s, giving up\n",
                    RECV_IDLE_MS / 1000u);
         }
         break;
      }

      ssize_t n = wait_datagram(fd, ms_until(now, deadline), buf, sizeof(buf));
      if (n < 0)
      {
         perror("error: recv");
         break;
      }
      if (n == 0 || pkt_decode(buf, (size_t)n, &pkt) != PKT_OK)
      {
         continue;
      }

      bool was_finished = rx.finished;
      gbn_recv_action_t act;
      gbn_receiver_on_packet(&rx, &pkt, &act);

      if (!was_finished)
      {
         deadline = now_ms() + RECV_IDLE_MS;
      }
      if (act.deliver && fwrite(act.payload, 1, act.payload_len, out) != act.payload_len)
      {
         perror("error: write");
         break;
      }
      if (act.send_ack)
      {
         packet_t ack = {.type = PKT_ACK, .seq = act.ack_seq, .length = 0};
         uint8_t wire[PKT_MAX_LEN];
         size_t wlen = pkt_encode(&ack, wire);
         if (!send_datagram(fd, wire, wlen))
         {
            break;
         }
      }
      if (act.finished && !was_finished)
      {
         /* First FIN: the file is complete; keep answering repeats for the linger period. */
         deadline = now_ms() + RECV_LINGER_MS;
      }
   }

   if (fclose(out) != 0)
   {
      perror("error: closing output file");
      status = EXIT_FAILURE_NET;
   }
   return status;
}

/* Reads the whole file into a heap buffer. Caller frees. */
static uint8_t *read_file(const char *path, size_t *len_out)
{
   FILE *in = fopen(path, "rb");
   if (in == NULL)
   {
      perror("error: cannot open input file");
      return NULL;
   }

   uint8_t *data = NULL;
   size_t len = 0;
   size_t cap = 65536;
   data = malloc(cap);
   while (data != NULL)
   {
      size_t n = fread(data + len, 1, cap - len, in);
      len += n;
      if (len < cap)
      {
         break;
      }
      uint8_t *bigger = realloc(data, cap * 2);
      if (bigger == NULL)
      {
         free(data);
         data = NULL;
         break;
      }
      data = bigger;
      cap *= 2;
   }

   if (data == NULL || ferror(in))
   {
      fprintf(stderr, "error: cannot read '%s'\n", path);
      free(data);
      data = NULL;
   }
   fclose(in);
   *len_out = len;
   return data;
}

static bool send_actions(int fd, const gbn_action_t *act)
{
   for (size_t i = 0; i < act->count; i++)
   {
      if (!send_datagram(fd, act->ptr[i], act->len[i]))
      {
         return false;
      }
   }
   return true;
}

static int run_send(int fd, const cli_config_t *cfg)
{
   size_t file_len = 0;
   uint8_t *data = read_file(cfg->file_path, &file_len);
   if (data == NULL)
   {
      return EXIT_FAILURE_NET;
   }

   gbn_sender_t *tx = malloc(sizeof(*tx));
   if (tx == NULL)
   {
      fprintf(stderr, "error: out of memory\n");
      free(data);
      return EXIT_FAILURE_NET;
   }

   int status = EXIT_FAILURE_NET;
   gbn_action_t act;
   uint8_t buf[PKT_MAX_LEN + 1];
   packet_t pkt;

   gbn_sender_init(tx, cfg->window, cfg->timeout_ms, data, file_len, now_ms(), &act);
   bool ok = send_actions(fd, &act);

   while (ok && !act.done && !act.failed)
   {
      uint64_t now = now_ms();
      if (now >= act.timer_deadline_ms)
      {
         gbn_sender_on_timeout(tx, now, &act);
         ok = send_actions(fd, &act);
         continue;
      }

      ssize_t n = wait_datagram(fd, ms_until(now, act.timer_deadline_ms), buf, sizeof(buf));
      if (n < 0)
      {
         perror("error: recv");
         ok = false;
      }
      else if (n > 0 && pkt_decode(buf, (size_t)n, &pkt) == PKT_OK && pkt.type == PKT_ACK)
      {
         gbn_sender_on_ack(tx, pkt.seq, now_ms(), &act);
         ok = send_actions(fd, &act);
      }
   }

   if (ok && act.done)
   {
      status = EXIT_OK;
   }
   else if (ok && act.failed)
   {
      fprintf(stderr, "error: no progress after 10 timeouts in a row, giving up\n");
   }

   free(tx);
   free(data);
   return status;
}

#ifdef TEST
#define main main_exclude
#endif

int main(int argc, char *argv[])
{
   cli_config_t cfg;
   switch (cli_parse(argc, argv, &cfg))
   {
   case CLI_OK:
      break;
   case CLI_USAGE:
      cli_print_usage(stdout);
      return EXIT_OK;
   case CLI_ERROR:
   default:
      cli_print_usage(stderr);
      return EXIT_BAD_USAGE;
   }

   int fd = open_relay_socket(cfg.relay_host, cfg.port);
   if (fd < 0)
   {
      return EXIT_FAILURE_NET;
   }

   int status = EXIT_FAILURE_NET;
   if (register_with_relay(fd, &cfg))
   {
      status = cfg.mode == CLI_MODE_RECV ? run_recv(fd, &cfg) : run_send(fd, &cfg);
   }

   close(fd);
   return status;
}

```

### packet.c

```c

#include "lab.h"

#include <arpa/inet.h>
#include <string.h>

uint16_t pkt_checksum(const uint8_t *data, size_t len)
{
   uint32_t sum = 0;
   size_t i = 0;

   for (; i + 1 < len; i += 2)
   {
      sum += (uint32_t)(((uint32_t)data[i] << 8) | (uint32_t)data[i + 1]);
   }

   if (i < len)
   {
      sum += (uint32_t)((uint32_t)data[i] << 8);
   }

   while (sum >> 16)
   {
      sum = (sum & 0xFFFFu) + (sum >> 16);
   }

   return (uint16_t)(~sum & 0xFFFFu);
}

size_t pkt_encode(const packet_t *pkt, uint8_t out[PKT_MAX_LEN])
{
   if (pkt->length > PKT_MAX_PAYLOAD)
   {
      return 0;
   }

   size_t total = (size_t)PKT_HEADER_LEN + (size_t)pkt->length;

   out[0] = (uint8_t)pkt->type;
   out[1] = 0;
   out[2] = 0;
   out[3] = 0;

   uint32_t seq_n = htonl(pkt->seq);
   memcpy(&out[4], &seq_n, sizeof(seq_n));

   uint16_t len_n = htons(pkt->length);
   memcpy(&out[8], &len_n, sizeof(len_n));

   memcpy(&out[PKT_HEADER_LEN], pkt->payload, pkt->length);

   uint16_t checksum = pkt_checksum(out, total);
   uint16_t checksum_n = htons(checksum);
   memcpy(&out[2], &checksum_n, sizeof(checksum_n));

   return total;
}

pkt_status_t pkt_decode(const uint8_t *in, size_t in_len, packet_t *out)
{
   if (in_len < PKT_HEADER_LEN)
   {
      return PKT_ERR_TOO_SHORT;
   }

   uint16_t len_n;
   memcpy(&len_n, &in[8], sizeof(len_n));
   uint16_t length = ntohs(len_n);

   if (length > PKT_MAX_PAYLOAD)
   {
      return PKT_ERR_PAYLOAD_TOO_LONG;
   }

   if ((size_t)PKT_HEADER_LEN + (size_t)length != in_len)
   {
      return PKT_ERR_LENGTH_MISMATCH;
   }

   uint8_t type_byte = in[0];
   if (type_byte > (uint8_t)PKT_FIN)
   {
      return PKT_ERR_BAD_TYPE;
   }

   if (in[1] != 0)
   {
      return PKT_ERR_BAD_RESERVED;
   }

   if (pkt_checksum(in, in_len) != 0)
   {
      return PKT_ERR_BAD_CHECKSUM;
   }

   uint32_t seq_n;
   memcpy(&seq_n, &in[4], sizeof(seq_n));

   out->type = (pkt_type_t)type_byte;
   out->seq = ntohl(seq_n);
   out->length = length;

   memcpy(out->payload, &in[PKT_HEADER_LEN], length);

   return PKT_OK;
}

```

## Tests Files
### lab-test.c

```c

#include "../src/lab.h"
#include "harness/unity.h"

#include <string.h>

void setUp(void)
{
}

void tearDown(void)
{
}

/* ======================================================================
 * Section 1-2: packet checksum / encode / decode
 * ====================================================================== */

static const uint8_t HI_PACKET[13] = {0x00, 0x00, 0x96, 0x91, 0x00, 0x00, 0x00,
                                      0x02, 0x00, 0x03, 0x48, 0x69, 0x21};
static const uint8_t ACK3_PACKET[10] = {0x01, 0x00, 0xfe, 0xfc, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00};

static void test_checksum_rfc1071_example(void)
{
   const uint8_t data[8] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
   TEST_ASSERT_EQUAL_HEX16(0x220d, pkt_checksum(data, sizeof(data)));
}

static void test_encode_hi_example_matches_literal(void)
{
   packet_t pkt;
   pkt.type = PKT_DATA;
   pkt.seq = 2;
   pkt.length = 3;
   memcpy(pkt.payload, "Hi!", 3);

   uint8_t out[PKT_MAX_LEN];
   size_t n = pkt_encode(&pkt, out);

   TEST_ASSERT_EQUAL_UINT32(13, n);
   TEST_ASSERT_EQUAL_MEMORY(HI_PACKET, out, 13);
}

static void test_decode_hi_example_round_trips(void)
{
   packet_t pkt;
   pkt_status_t st = pkt_decode(HI_PACKET, sizeof(HI_PACKET), &pkt);

   TEST_ASSERT_EQUAL_INT(PKT_OK, st);
   TEST_ASSERT_EQUAL_INT(PKT_DATA, pkt.type);
   TEST_ASSERT_EQUAL_UINT32(2, pkt.seq);
   TEST_ASSERT_EQUAL_UINT16(3, pkt.length);
   TEST_ASSERT_EQUAL_MEMORY("Hi!", pkt.payload, 3);
}

static void test_decode_hi_example_single_bit_flip_fails_checksum(void)
{
   uint8_t corrupted[13];
   memcpy(corrupted, HI_PACKET, sizeof(HI_PACKET));
   corrupted[10] ^= 0x01u; /* flip one bit of the payload */

   packet_t pkt;
   pkt_status_t st = pkt_decode(corrupted, sizeof(corrupted), &pkt);

   TEST_ASSERT_EQUAL_INT(PKT_ERR_BAD_CHECKSUM, st);
}

static void test_decode_ack3_literal(void)
{
   packet_t pkt;
   pkt_status_t st = pkt_decode(ACK3_PACKET, sizeof(ACK3_PACKET), &pkt);

   TEST_ASSERT_EQUAL_INT(PKT_OK, st);
   TEST_ASSERT_EQUAL_INT(PKT_ACK, pkt.type);
   TEST_ASSERT_EQUAL_UINT32(3, pkt.seq);
   TEST_ASSERT_EQUAL_UINT16(0, pkt.length);
}

static void test_encode_ack3_matches_literal(void)
{
   packet_t pkt;
   pkt.type = PKT_ACK;
   pkt.seq = 3;
   pkt.length = 0;

   uint8_t out[PKT_MAX_LEN];
   size_t n = pkt_encode(&pkt, out);

   TEST_ASSERT_EQUAL_UINT32(10, n);
   TEST_ASSERT_EQUAL_MEMORY(ACK3_PACKET, out, 10);
}

static void test_decode_too_short(void)
{
   const uint8_t buf[9] = {0};
   packet_t pkt;
   TEST_ASSERT_EQUAL_INT(PKT_ERR_TOO_SHORT, pkt_decode(buf, sizeof(buf), &pkt));
}

static void test_decode_length_mismatch(void)
{
   /* header claims length=3 but the datagram is only 10 bytes (no payload) */
   uint8_t buf[10];
   memcpy(buf, ACK3_PACKET, 10);
   buf[8] = 0x00;
   buf[9] = 0x03; /* length field now says 3, but buf is only 10 bytes */
   packet_t pkt;
   TEST_ASSERT_EQUAL_INT(PKT_ERR_LENGTH_MISMATCH, pkt_decode(buf, 10, &pkt));
}

static void test_decode_payload_too_long(void)
{
   /* length field = 1025 (> 1024), with a CONSISTENT datagram size (10+1025),
    * so this specifically exercises PAYLOAD_TOO_LONG rather than
    * LENGTH_MISMATCH -- the two checks must stay distinguishable. */
   static uint8_t buf[10 + 1025];
   memset(buf, 0, sizeof(buf));
   buf[0] = PKT_DATA;
   buf[8] = (uint8_t)(1025 >> 8);
   buf[9] = (uint8_t)(1025 & 0xFF);
   packet_t pkt;
   TEST_ASSERT_EQUAL_INT(PKT_ERR_PAYLOAD_TOO_LONG, pkt_decode(buf, sizeof(buf), &pkt));
}

static void test_decode_bad_type(void)
{
   uint8_t buf[10];
   memcpy(buf, ACK3_PACKET, 10);
   buf[0] = 3; /* no such type */
   /* checksum will also now be wrong, but type is checked first */
   packet_t pkt;
   TEST_ASSERT_EQUAL_INT(PKT_ERR_BAD_TYPE, pkt_decode(buf, 10, &pkt));
}

static void test_decode_bad_reserved(void)
{
   uint8_t buf[10];
   memcpy(buf, ACK3_PACKET, 10);
   buf[1] = 1; /* reserved must be 0 */
   packet_t pkt;
   TEST_ASSERT_EQUAL_INT(PKT_ERR_BAD_RESERVED, pkt_decode(buf, 10, &pkt));
}

static void test_decode_bad_checksum(void)
{
   uint8_t buf[10];
   memcpy(buf, ACK3_PACKET, 10);
   buf[2] ^= 0xFFu; /* corrupt only the checksum field itself */
   packet_t pkt;
   TEST_ASSERT_EQUAL_INT(PKT_ERR_BAD_CHECKSUM, pkt_decode(buf, 10, &pkt));
}

static void test_encode_payload_too_long_returns_zero(void)
{
   packet_t pkt;
   pkt.type = PKT_DATA;
   pkt.seq = 0;
   pkt.length = PKT_MAX_PAYLOAD + 1; /* invalid: caller's bug, must be rejected */
   uint8_t out[PKT_MAX_LEN];
   TEST_ASSERT_EQUAL_UINT32(0, pkt_encode(&pkt, out));
}

/* ======================================================================
 * Section 3: receiver state machine
 * ====================================================================== */

static void test_receiver_in_order_delivery(void)
{
   gbn_receiver_t r;
   gbn_recv_action_t out;
   gbn_receiver_init(&r);

   packet_t d0;
   d0.type = PKT_DATA;
   d0.seq = 0;
   d0.length = 2;
   memcpy(d0.payload, "ab", 2);
   gbn_receiver_on_packet(&r, &d0, &out);
   TEST_ASSERT_TRUE(out.deliver);
   TEST_ASSERT_EQUAL_MEMORY("ab", out.payload, 2);
   TEST_ASSERT_TRUE(out.send_ack);
   TEST_ASSERT_EQUAL_UINT32(1, out.ack_seq);
   TEST_ASSERT_EQUAL_UINT32(1, r.expected);

   packet_t d1;
   d1.type = PKT_DATA;
   d1.seq = 1;
   d1.length = 2;
   memcpy(d1.payload, "cd", 2);
   gbn_receiver_on_packet(&r, &d1, &out);
   TEST_ASSERT_TRUE(out.deliver);
   TEST_ASSERT_EQUAL_UINT32(2, out.ack_seq);

   packet_t fin;
   fin.type = PKT_FIN;
   fin.seq = 2;
   fin.length = 0;
   gbn_receiver_on_packet(&r, &fin, &out);
   TEST_ASSERT_FALSE(out.deliver);
   TEST_ASSERT_TRUE(out.finished);
   TEST_ASSERT_EQUAL_UINT32(3, out.ack_seq);
}

static void test_receiver_duplicate_data_is_discarded(void)
{
   gbn_receiver_t r;
   gbn_recv_action_t out;
   gbn_receiver_init(&r);

   packet_t d0;
   d0.type = PKT_DATA;
   d0.seq = 0;
   d0.length = 1;
   d0.payload[0] = 'x';
   gbn_receiver_on_packet(&r, &d0, &out);

   gbn_receiver_on_packet(&r, &d0, &out); /* same packet again */
   TEST_ASSERT_FALSE(out.deliver);
   TEST_ASSERT_TRUE(out.send_ack);
   TEST_ASSERT_EQUAL_UINT32(1, out.ack_seq);
   TEST_ASSERT_EQUAL_UINT32(1, r.expected);
}

static void test_receiver_data_past_a_gap_is_discarded(void)
{
   gbn_receiver_t r;
   gbn_recv_action_t out;
   gbn_receiver_init(&r);

   packet_t d5;
   d5.type = PKT_DATA;
   d5.seq = 5;
   d5.length = 1;
   d5.payload[0] = 'z';
   gbn_receiver_on_packet(&r, &d5, &out);

   TEST_ASSERT_FALSE(out.deliver);
   TEST_ASSERT_EQUAL_UINT32(0, out.ack_seq);
   TEST_ASSERT_EQUAL_UINT32(0, r.expected);
}

static void test_receiver_repeated_fin_after_finished(void)
{
   gbn_receiver_t r;
   gbn_recv_action_t out;
   gbn_receiver_init(&r);
   r.expected = 2; /* pretend DATA 0,1 already delivered */

   packet_t fin;
   fin.type = PKT_FIN;
   fin.seq = 2;
   fin.length = 0;

   gbn_receiver_on_packet(&r, &fin, &out);
   TEST_ASSERT_TRUE(out.finished);
   TEST_ASSERT_EQUAL_UINT32(3, out.ack_seq);
   TEST_ASSERT_EQUAL_UINT32(3, r.expected);

   /* repeat: the sender never saw our ACK and resent the same FIN */
   gbn_receiver_on_packet(&r, &fin, &out);
   TEST_ASSERT_TRUE(out.finished);
   TEST_ASSERT_FALSE(out.deliver);
   TEST_ASSERT_EQUAL_UINT32(3, out.ack_seq); /* same ACK, not re-advanced */
   TEST_ASSERT_EQUAL_UINT32(3, r.expected);  /* expected did not move again */
}

static void test_receiver_data_during_linger_is_discarded(void)
{
   gbn_receiver_t r;
   gbn_recv_action_t out;
   gbn_receiver_init(&r);
   r.expected = 3;
   r.finished = true; /* simulate: already in the post-FIN linger */

   packet_t old_data;
   old_data.type = PKT_DATA;
   old_data.seq = 1; /* an old duplicate arriving late */
   old_data.length = 1;
   old_data.payload[0] = 'q';

   gbn_receiver_on_packet(&r, &old_data, &out);
   TEST_ASSERT_FALSE(out.deliver);
   TEST_ASSERT_TRUE(out.finished);
   TEST_ASSERT_EQUAL_UINT32(3, out.ack_seq);
}

static void test_receiver_stray_ack_is_noop(void)
{
   gbn_receiver_t r;
   gbn_recv_action_t out;
   gbn_receiver_init(&r);

   packet_t ack;
   ack.type = PKT_ACK;
   ack.seq = 7;
   ack.length = 0;

   gbn_receiver_on_packet(&r, &ack, &out);
   TEST_ASSERT_FALSE(out.deliver);
   TEST_ASSERT_FALSE(out.send_ack);
   TEST_ASSERT_EQUAL_UINT32(0, r.expected);
}

/* ======================================================================
 * Section 4-5: sender state machine
 * ====================================================================== */

static void fill_pattern(uint8_t *buf, size_t len, uint8_t seed)
{
   for (size_t i = 0; i < len; i++)
   {
      buf[i] = (uint8_t)((size_t)seed + i * 7u);
   }
}

static void test_sender_small_file_single_burst(void)
{
   uint8_t file[100];
   fill_pattern(file, sizeof(file), 1);

   gbn_sender_t s;
   gbn_action_t out;
   gbn_sender_init(&s, 8, 250, file, sizeof(file), 0, &out);

   /* 100-byte file -> 1 DATA packet + 1 FIN = total 2, both fit in window 8 */
   TEST_ASSERT_EQUAL_UINT32(2, s.total);
   TEST_ASSERT_EQUAL_UINT32(2, s.next);
   TEST_ASSERT_EQUAL_UINT32(2, out.count);
   TEST_ASSERT_TRUE(out.timer_running);
   TEST_ASSERT_FALSE(out.done);
}

static void test_sender_window_full_sends_exactly_window_packets(void)
{
   uint8_t file[10 * 1024];
   fill_pattern(file, sizeof(file), 2);

   gbn_sender_t s;
   gbn_action_t out;
   gbn_sender_init(&s, 4, 250, file, sizeof(file), 0, &out);

   TEST_ASSERT_EQUAL_UINT32(11, s.total); /* 10 DATA + 1 FIN */
   TEST_ASSERT_EQUAL_UINT32(4, out.count);
   TEST_ASSERT_EQUAL_UINT32(4, s.next);
   TEST_ASSERT_EQUAL_UINT32(0, s.base);
}

static void test_sender_cumulative_ack_slides_and_sends_more(void)
{
   uint8_t file[10 * 1024];
   fill_pattern(file, sizeof(file), 3);

   gbn_sender_t s;
   gbn_action_t out;
   gbn_sender_init(&s, 4, 250, file, sizeof(file), 0, &out);

   /* ACK 3 acknowledges DATA 0,1,2 in one step (cumulative) */
   gbn_sender_on_ack(&s, 3, 10, &out);
   TEST_ASSERT_EQUAL_UINT32(3, s.base);
   TEST_ASSERT_EQUAL_UINT32(7, s.next); /* window re-filled: 4,5,6 newly sent */
   TEST_ASSERT_EQUAL_UINT32(3, out.count);
}

static void test_sender_stale_ack_is_noop(void)
{
   uint8_t file[10 * 1024];
   fill_pattern(file, sizeof(file), 4);

   gbn_sender_t s;
   gbn_action_t out;
   gbn_sender_init(&s, 4, 250, file, sizeof(file), 0, &out);
   gbn_sender_on_ack(&s, 3, 10, &out);

   uint32_t base_before = s.base, next_before = s.next;
   gbn_sender_on_ack(&s, 2, 20, &out); /* ack_seq <= base: duplicate */
   TEST_ASSERT_EQUAL_UINT32(0, out.count);
   TEST_ASSERT_EQUAL_UINT32(base_before, s.base);
   TEST_ASSERT_EQUAL_UINT32(next_before, s.next);

   gbn_sender_on_ack(&s, 3, 30, &out); /* ack_seq == base: also a duplicate */
   TEST_ASSERT_EQUAL_UINT32(0, out.count);

   gbn_sender_on_ack(&s, next_before + 1u, 40, &out); /* beyond next: never sent, ignored */
   TEST_ASSERT_EQUAL_UINT32(0, out.count);
   TEST_ASSERT_EQUAL_UINT32(base_before, s.base);
}

static void test_sender_timeout_resend_is_byte_identical(void)
{
   uint8_t file[3 * 1024];
   fill_pattern(file, sizeof(file), 5);

   gbn_sender_t s;
   gbn_action_t first, second;
   gbn_sender_init(&s, 8, 250, file, sizeof(file), 0, &first);

   TEST_ASSERT_EQUAL_UINT32(4, first.count); /* 3 DATA + FIN, all fit in window 8 */

   uint8_t saved[4][PKT_MAX_LEN];
   uint16_t saved_len[4];
   for (size_t i = 0; i < first.count; i++)
   {
      memcpy(saved[i], first.ptr[i], first.len[i]);
      saved_len[i] = first.len[i];
   }

   gbn_sender_on_timeout(&s, 260, &second);
   TEST_ASSERT_EQUAL_UINT32(4, second.count);
   for (size_t i = 0; i < second.count; i++)
   {
      TEST_ASSERT_EQUAL_UINT16(saved_len[i], second.len[i]);
      TEST_ASSERT_EQUAL_MEMORY(saved[i], second.ptr[i], saved_len[i]);
   }
}

static void test_sender_ten_consecutive_timeouts_fails(void)
{
   uint8_t file[1024];
   fill_pattern(file, sizeof(file), 6);

   gbn_sender_t s;
   gbn_action_t out;
   gbn_sender_init(&s, 8, 100, file, sizeof(file), 0, &out);

   uint64_t now = 100;
   for (int i = 1; i <= 9; i++)
   {
      gbn_sender_on_timeout(&s, now, &out);
      TEST_ASSERT_FALSE(out.failed);
      now += 100;
   }
   gbn_sender_on_timeout(&s, now, &out); /* 10th consecutive timeout */
   TEST_ASSERT_TRUE(out.failed);
   TEST_ASSERT_EQUAL_UINT32(0, out.count);
}

static void test_sender_progress_resets_timeout_counter(void)
{
   uint8_t file[10 * 1024];
   fill_pattern(file, sizeof(file), 7);

   gbn_sender_t s;
   gbn_action_t out;
   gbn_sender_init(&s, 4, 100, file, sizeof(file), 0, &out);

   uint64_t now = 100;
   for (int i = 1; i <= 5; i++)
   {
      gbn_sender_on_timeout(&s, now, &out);
      now += 100;
   }
   TEST_ASSERT_FALSE(out.failed);

   gbn_sender_on_ack(&s, 1, now, &out); /* progress: moves base, resets counter */
   now += 100;
   TEST_ASSERT_EQUAL_UINT32(0, s.consecutive_timeouts);

   for (int i = 1; i <= 9; i++)
   {
      gbn_sender_on_timeout(&s, now, &out);
      TEST_ASSERT_FALSE(out.failed); /* only 9 since the reset: must not fail yet */
      now += 100;
   }
   gbn_sender_on_timeout(&s, now, &out); /* the 10th since the reset */
   TEST_ASSERT_TRUE(out.failed);
}

static void test_sender_empty_file_sends_lone_fin(void)
{
   gbn_sender_t s;
   gbn_action_t out;
   gbn_sender_init(&s, 8, 250, NULL, 0, 0, &out);

   TEST_ASSERT_EQUAL_UINT32(1, s.total);
   TEST_ASSERT_EQUAL_UINT32(1, out.count);

   packet_t pkt;
   TEST_ASSERT_EQUAL_INT(PKT_OK, pkt_decode(out.ptr[0], out.len[0], &pkt));
   TEST_ASSERT_EQUAL_INT(PKT_FIN, pkt.type);
   TEST_ASSERT_EQUAL_UINT32(0, pkt.seq);

   gbn_sender_on_ack(&s, 1, 10, &out);
   TEST_ASSERT_TRUE(out.done);
}

static void test_sender_exact_multiple_of_1024_has_no_trailing_short_packet(void)
{
   uint8_t file[2048];
   fill_pattern(file, sizeof(file), 8);

   gbn_sender_t s;
   gbn_action_t out;
   gbn_sender_init(&s, 8, 250, file, sizeof(file), 0, &out);

   TEST_ASSERT_EQUAL_UINT32(3, s.total); /* DATA 0, DATA 1, FIN 2 -- no DATA 2 */
   TEST_ASSERT_EQUAL_UINT32(3, out.count);

   packet_t pkt;
   TEST_ASSERT_EQUAL_INT(PKT_OK, pkt_decode(out.ptr[1], out.len[1], &pkt));
   TEST_ASSERT_EQUAL_INT(PKT_DATA, pkt.type);
   TEST_ASSERT_EQUAL_UINT32(1, pkt.seq);
   TEST_ASSERT_EQUAL_UINT16(1024, pkt.length); /* full, not a short trailing packet */

   TEST_ASSERT_EQUAL_INT(PKT_OK, pkt_decode(out.ptr[2], out.len[2], &pkt));
   TEST_ASSERT_EQUAL_INT(PKT_FIN, pkt.type);
   TEST_ASSERT_EQUAL_UINT32(2, pkt.seq);
}

/* ======================================================================
 * Section 6: CLI parsing
 * ====================================================================== */

static void test_cli_no_args_is_usage(void)
{
   char *argv[] = {"myapp"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_USAGE, cli_parse(1, argv, &cfg));
}

static void test_cli_valid_send_all_flags(void)
{
   char *argv[] = {"myapp", "send", "-s",   "abc-1", "-w",  "16", "-T",   "500",       "-l",
                   "0.1",   "-c",   "0.05", "-d",    "0.2", "-p", "5000", "relayhost", "file.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_OK, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   TEST_ASSERT_EQUAL_INT(CLI_MODE_SEND, cfg.mode);
   TEST_ASSERT_EQUAL_STRING("abc-1", cfg.session);
   TEST_ASSERT_EQUAL_UINT32(16, cfg.window);
   TEST_ASSERT_EQUAL_UINT32(500, cfg.timeout_ms);
   TEST_ASSERT_EQUAL_UINT16(5000, cfg.port);
   TEST_ASSERT_EQUAL_STRING("relayhost", cfg.relay_host);
   TEST_ASSERT_EQUAL_STRING("file.bin", cfg.file_path);
}

static void test_cli_valid_send_minimal_uses_defaults(void)
{
   char *argv[] = {"myapp", "send", "-s", "sess", "relayhost", "file.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_OK, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   TEST_ASSERT_EQUAL_UINT32(8, cfg.window);
   TEST_ASSERT_EQUAL_UINT32(250, cfg.timeout_ms);
   TEST_ASSERT_EQUAL_UINT16(4250, cfg.port);
}

static void test_cli_valid_recv(void)
{
   char *argv[] = {"myapp", "recv", "-s", "sess", "relayhost", "out.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_OK, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   TEST_ASSERT_EQUAL_INT(CLI_MODE_RECV, cfg.mode);
}

static void test_cli_missing_session_is_error(void)
{
   char *argv[] = {"myapp", "send", "relayhost", "file.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_ERROR, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
}

static void test_cli_window_out_of_range_is_error(void)
{
   cli_config_t cfg;
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-w", "0", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-w", "65", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
}

static void test_cli_rate_out_of_range_is_error(void)
{
   char *argv[] = {"myapp", "send", "-s", "sess", "-l", "0.6", "relayhost", "file.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_ERROR, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
}

static void test_cli_wrong_positional_count_is_error(void)
{
   cli_config_t cfg;
   {
      char *argv[] = {"myapp", "send", "-s", "sess"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "onlyone"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "a", "b", "c"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
}

static void test_cli_invalid_session_chars_is_error(void)
{
   char *argv[] = {"myapp", "send", "-s", "Bad_Name!", "relayhost", "file.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_ERROR, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
}

static void test_cli_unknown_mode_is_error(void)
{
   char *argv[] = {"myapp", "foo", "-s", "sess", "relayhost", "file.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_ERROR, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
}

static void test_cli_send_only_flag_on_recv_is_error(void)
{
   char *argv[] = {"myapp", "recv", "-s", "sess", "-w", "16", "relayhost", "out.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_ERROR, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
}

static void test_cli_session_too_long_is_error(void)
{
   char *argv[] = {"myapp",     "send",    "-s", "this-session-name-is-way-too-long-at-33-chars",
                   "relayhost", "file.bin"};
   cli_config_t cfg;
   TEST_ASSERT_EQUAL_INT(CLI_ERROR, cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
}

static void test_cli_malformed_numbers_are_errors(void)
{
   const char *bad[][2] = {{"-s", ""},    {"-w", "4x"}, {"-l", "0.1x"}, {"-l", "-0.1"},
                           {"-l", "nan"}, {"-T", "0"},  {"-p", "0"}};
   for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
   {
      cli_config_t cfg;
      char *argv[] = {"myapp",           "send",      "-s",      "sess", (char *)bad[i][0],
                      (char *)bad[i][1], "relayhost", "file.bin"};
      if (strcmp(bad[i][0], "-s") == 0)
      {
         argv[3] = (char *)bad[i][1]; /* replace the valid session instead of duplicating it */
         argv[4] = "-w";
         argv[5] = "4";
      }
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
}

static void test_cli_empty_flag_value_is_error(void)
{
   cli_config_t cfg;
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-w", "", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-l", "", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-l", "abc", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
}

static void test_cli_each_flags_own_parse_failure_is_error(void)
{
   cli_config_t cfg;
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-T", "abc", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-c", "0.9", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-d", "0.9", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
   {
      char *argv[] = {"myapp", "send", "-s", "sess", "-p", "100000", "relayhost", "file.bin"};
      TEST_ASSERT_EQUAL_INT(CLI_ERROR,
                            cli_parse((int)(sizeof(argv) / sizeof(argv[0])), argv, &cfg));
   }
}

static void test_cli_print_usage_does_not_crash(void)
{
   FILE *f = tmpfile();
   TEST_ASSERT_NOT_NULL(f);
   cli_print_usage(f);
   long pos = ftell(f);
   TEST_ASSERT_GREATER_THAN(0, pos);
   fclose(f);
}

static void test_cli_repeated_calls_reset_getopt_state(void)
{
   cli_config_t cfg;

   char *argv1[] = {"myapp", "send", "-s", "first", "relayhost", "file.bin"};
   TEST_ASSERT_EQUAL_INT(CLI_OK, cli_parse((int)(sizeof(argv1) / sizeof(argv1[0])), argv1, &cfg));
   TEST_ASSERT_EQUAL_STRING("first", cfg.session);

   char *argv2[] = {"myapp", "recv", "-s", "second", "relayhost", "out.bin"};
   TEST_ASSERT_EQUAL_INT(CLI_OK, cli_parse((int)(sizeof(argv2) / sizeof(argv2[0])), argv2, &cfg));
   TEST_ASSERT_EQUAL_STRING("second", cfg.session);
   TEST_ASSERT_EQUAL_INT(CLI_MODE_RECV, cfg.mode);

   char *argv3[] = {"myapp"};
   TEST_ASSERT_EQUAL_INT(CLI_USAGE, cli_parse(1, argv3, &cfg));

   char *argv4[] = {"myapp", "send", "-s", "third", "-w", "32", "relayhost", "file.bin"};
   TEST_ASSERT_EQUAL_INT(CLI_OK, cli_parse((int)(sizeof(argv4) / sizeof(argv4[0])), argv4, &cfg));
   TEST_ASSERT_EQUAL_STRING("third", cfg.session);
   TEST_ASSERT_EQUAL_UINT32(32, cfg.window);
}

/* ======================================================================
 * Section 7: seeded 20%-loss end-to-end simulation
 * ====================================================================== */

static uint32_t prng_next(uint64_t *state)
{
   *state = *state * 6364136223846793005ULL + 1442695040888963407ULL;
   return (uint32_t)(*state >> 33);
}

static double prng_unit(uint64_t *state)
{
   return (double)prng_next(state) / 4294967296.0;
}

typedef struct
{
   uint8_t data[PKT_MAX_LEN];
   uint16_t len;
   uint64_t arrival;
} sim_item_t;

typedef struct
{
   sim_item_t items[128];
   size_t count;
} sim_queue_t;

static void sq_push(sim_queue_t *q, const uint8_t *data, uint16_t len, uint64_t arrival)
{
   TEST_ASSERT_LESS_THAN_MESSAGE(128, q->count, "simulation queue overflowed");
   memcpy(q->items[q->count].data, data, len);
   q->items[q->count].len = len;
   q->items[q->count].arrival = arrival;
   q->count += 1u;
}

static sim_item_t sq_pop_front(sim_queue_t *q)
{
   sim_item_t front = q->items[0];
   for (size_t i = 1; i < q->count; i++)
   {
      q->items[i - 1] = q->items[i];
   }
   q->count -= 1u;
   return front;
}

/* Mirrors the relay's own roll order: drop, else corrupt, else duplicate,
 * else pass through -- each a fresh, independent draw, at p=0.2 each. */
static size_t sim_channel(uint64_t *rng, const uint8_t *in, uint16_t in_len,
                          uint8_t out[2][PKT_MAX_LEN], uint16_t out_len[2])
{
   if (prng_unit(rng) < 0.2)
   {
      return 0; /* dropped */
   }
   if (prng_unit(rng) < 0.2)
   {
      memcpy(out[0], in, in_len);
      uint32_t bit = prng_next(rng) % ((uint32_t)in_len * 8u);
      out[0][bit / 8u] ^= (uint8_t)(1u << (bit % 8u));
      out_len[0] = in_len;
      return 1;
   }
   if (prng_unit(rng) < 0.2)
   {
      memcpy(out[0], in, in_len);
      memcpy(out[1], in, in_len);
      out_len[0] = in_len;
      out_len[1] = in_len;
      return 2;
   }
   memcpy(out[0], in, in_len);
   out_len[0] = in_len;
   return 1;
}

static void sim_push_damaged(sim_queue_t *q, uint64_t *rng, const uint8_t *data, uint16_t len,
                             uint64_t arrival)
{
   uint8_t dmg[2][PKT_MAX_LEN];
   uint16_t dmg_len[2];
   size_t n = sim_channel(rng, data, len, dmg, dmg_len);
   for (size_t k = 0; k < n; k++)
   {
      sq_push(q, dmg[k], dmg_len[k], arrival);
   }
}

static void run_seeded_e2e_transfer(uint64_t seed, bool *out_converged, bool *out_failed,
                                    bool *out_match)
{
   enum
   {
      FILE_LEN = 4500,
      WINDOW = 4,
      TIMEOUT_MS = 50,
      LINK_DELAY_MS = 5,
      MAX_ITERS = 20000,
      MAX_SIM_TIME_MS = 120000
   };

   static uint8_t file_data[FILE_LEN];
   static uint8_t received[FILE_LEN];
   for (size_t i = 0; i < FILE_LEN; i++)
   {
      file_data[i] = (uint8_t)(i * 31u + 7u);
   }
   memset(received, 0, sizeof(received));

   uint64_t rng = seed;

   gbn_sender_t sender;
   gbn_receiver_t receiver;
   gbn_action_t s_action;
   gbn_recv_action_t r_action;

   gbn_receiver_init(&receiver);
   gbn_sender_init(&sender, WINDOW, TIMEOUT_MS, file_data, FILE_LEN, 0, &s_action);

   static sim_queue_t fwd, rev;
   fwd.count = 0;
   rev.count = 0;
   uint64_t now = 0;

   for (size_t i = 0; i < s_action.count; i++)
   {
      sim_push_damaged(&fwd, &rng, s_action.ptr[i], s_action.len[i], now + LINK_DELAY_MS);
   }

   bool converged = false;
   int iterations = 0;
   while (iterations++ < MAX_ITERS && now <= MAX_SIM_TIME_MS)
   {
      bool have_event = false;
      uint64_t next_time = 0;

      if (sender.timer_running)
      {
         next_time = sender.timer_deadline_ms;
         have_event = true;
      }
      if (fwd.count > 0 && (!have_event || fwd.items[0].arrival < next_time))
      {
         next_time = fwd.items[0].arrival;
         have_event = true;
      }
      if (rev.count > 0 && (!have_event || rev.items[0].arrival < next_time))
      {
         next_time = rev.items[0].arrival;
         have_event = true;
      }

      if (!have_event)
      {
         break; /* nothing pending and not done: stuck */
      }
      now = next_time;

      if (sender.timer_running && sender.timer_deadline_ms == now)
      {
         gbn_sender_on_timeout(&sender, now, &s_action);
         for (size_t i = 0; i < s_action.count; i++)
         {
            sim_push_damaged(&fwd, &rng, s_action.ptr[i], s_action.len[i], now + LINK_DELAY_MS);
         }
         if (s_action.failed)
         {
            break;
         }
      }

      while (fwd.count > 0 && fwd.items[0].arrival == now)
      {
         sim_item_t item = sq_pop_front(&fwd);
         packet_t pkt;
         if (pkt_decode(item.data, item.len, &pkt) == PKT_OK)
         {
            gbn_receiver_on_packet(&receiver, &pkt, &r_action);
            if (r_action.deliver)
            {
               memcpy(received + (size_t)pkt.seq * 1024u, r_action.payload, r_action.payload_len);
            }
            if (r_action.send_ack)
            {
               packet_t ack;
               ack.type = PKT_ACK;
               ack.seq = r_action.ack_seq;
               ack.length = 0;
               uint8_t wire[PKT_MAX_LEN];
               size_t wlen = pkt_encode(&ack, wire);
               sim_push_damaged(&rev, &rng, wire, (uint16_t)wlen, now + LINK_DELAY_MS);
            }
         }
      }

      while (rev.count > 0 && rev.items[0].arrival == now)
      {
         sim_item_t item = sq_pop_front(&rev);
         packet_t pkt;
         if (pkt_decode(item.data, item.len, &pkt) == PKT_OK && pkt.type == PKT_ACK)
         {
            gbn_sender_on_ack(&sender, pkt.seq, now, &s_action);
            for (size_t i = 0; i < s_action.count; i++)
            {
               sim_push_damaged(&fwd, &rng, s_action.ptr[i], s_action.len[i], now + LINK_DELAY_MS);
            }
            if (s_action.failed)
            {
               break;
            }
         }
      }

      if (s_action.done)
      {
         converged = true;
         break;
      }
      if (s_action.failed)
      {
         break;
      }
   }

   *out_converged = converged;
   *out_failed = sender.failed;
   *out_match = (memcmp(file_data, received, FILE_LEN) == 0);
}

static void test_e2e_seeded_lossy_transfer(void)
{
   bool converged, failed, match;
   run_seeded_e2e_transfer(0x1234567890ABCDEFULL, &converged, &failed, &match);

   TEST_ASSERT_TRUE_MESSAGE(converged, "simulation did not converge within iteration/time caps");
   TEST_ASSERT_FALSE_MESSAGE(failed, "sender gave up after 10 consecutive timeouts");
   TEST_ASSERT_TRUE_MESSAGE(match, "delivered bytes did not match the original file");
}

int main(void)
{
   UNITY_BEGIN();

   RUN_TEST(test_checksum_rfc1071_example);
   RUN_TEST(test_encode_hi_example_matches_literal);
   RUN_TEST(test_decode_hi_example_round_trips);
   RUN_TEST(test_decode_hi_example_single_bit_flip_fails_checksum);
   RUN_TEST(test_decode_ack3_literal);
   RUN_TEST(test_encode_ack3_matches_literal);
   RUN_TEST(test_decode_too_short);
   RUN_TEST(test_decode_length_mismatch);
   RUN_TEST(test_decode_payload_too_long);
   RUN_TEST(test_decode_bad_type);
   RUN_TEST(test_decode_bad_reserved);
   RUN_TEST(test_decode_bad_checksum);
   RUN_TEST(test_encode_payload_too_long_returns_zero);

   RUN_TEST(test_receiver_in_order_delivery);
   RUN_TEST(test_receiver_duplicate_data_is_discarded);
   RUN_TEST(test_receiver_data_past_a_gap_is_discarded);
   RUN_TEST(test_receiver_repeated_fin_after_finished);
   RUN_TEST(test_receiver_data_during_linger_is_discarded);
   RUN_TEST(test_receiver_stray_ack_is_noop);

   RUN_TEST(test_sender_small_file_single_burst);
   RUN_TEST(test_sender_window_full_sends_exactly_window_packets);
   RUN_TEST(test_sender_cumulative_ack_slides_and_sends_more);
   RUN_TEST(test_sender_stale_ack_is_noop);
   RUN_TEST(test_sender_timeout_resend_is_byte_identical);
   RUN_TEST(test_sender_ten_consecutive_timeouts_fails);
   RUN_TEST(test_sender_progress_resets_timeout_counter);
   RUN_TEST(test_sender_empty_file_sends_lone_fin);
   RUN_TEST(test_sender_exact_multiple_of_1024_has_no_trailing_short_packet);

   RUN_TEST(test_cli_no_args_is_usage);
   RUN_TEST(test_cli_valid_send_all_flags);
   RUN_TEST(test_cli_valid_send_minimal_uses_defaults);
   RUN_TEST(test_cli_valid_recv);
   RUN_TEST(test_cli_missing_session_is_error);
   RUN_TEST(test_cli_window_out_of_range_is_error);
   RUN_TEST(test_cli_rate_out_of_range_is_error);
   RUN_TEST(test_cli_wrong_positional_count_is_error);
   RUN_TEST(test_cli_invalid_session_chars_is_error);
   RUN_TEST(test_cli_unknown_mode_is_error);
   RUN_TEST(test_cli_send_only_flag_on_recv_is_error);
   RUN_TEST(test_cli_session_too_long_is_error);
   RUN_TEST(test_cli_malformed_numbers_are_errors);
   RUN_TEST(test_cli_empty_flag_value_is_error);
   RUN_TEST(test_cli_each_flags_own_parse_failure_is_error);
   RUN_TEST(test_cli_print_usage_does_not_crash);
   RUN_TEST(test_cli_repeated_calls_reset_getopt_state);

   RUN_TEST(test_e2e_seeded_lossy_transfer);

   return UNITY_END();
}

```

## Scripts Files
Report generated on 10/12/2026 at 02:27:38


---

## End of Report

SHA-256 Hash of the report: 8b727d7a39b08d1c219d7e0ed115333bc164ad04a36b42ea6e7f336a750049e3

Do not edit the generated report. Any changes will be reported as academic dishonesty

---
## GitHub Info
- GitHub repo name: harrisonjansenva/cs425-p2
- The repository visibility is public.
- The workflow was triggered by harrisonjansenva
