#include "lab.h"

void gbn_sender_init(gbn_sender_t *s, uint32_t window, uint64_t timeout_ms,
                     const uint8_t *file_data, size_t file_len, uint64_t now_ms, gbn_action_t *out)
{
   (void)s;
   (void)window;
   (void)timeout_ms;
   (void)file_data;
   (void)file_len;
   (void)now_ms;
   (void)out;
}

void gbn_sender_on_ack(gbn_sender_t *s, uint32_t ack_seq, uint64_t now_ms, gbn_action_t *out)
{
   (void)s;
   (void)ack_seq;
   (void)now_ms;
   (void)out;
}

void gbn_sender_on_timeout(gbn_sender_t *s, uint64_t now_ms, gbn_action_t *out)
{
   (void)s;
   (void)now_ms;
   (void)out;
}
