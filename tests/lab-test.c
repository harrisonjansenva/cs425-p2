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
   RUN_TEST(test_cli_empty_flag_value_is_error);
   RUN_TEST(test_cli_each_flags_own_parse_failure_is_error);
   RUN_TEST(test_cli_print_usage_does_not_crash);
   RUN_TEST(test_cli_repeated_calls_reset_getopt_state);

   RUN_TEST(test_e2e_seeded_lossy_transfer);

   return UNITY_END();
}
