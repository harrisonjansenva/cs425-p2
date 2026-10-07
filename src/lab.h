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
