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
