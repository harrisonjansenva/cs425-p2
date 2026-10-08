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
   if (ack_seq > s->base)
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
