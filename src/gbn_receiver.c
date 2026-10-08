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
