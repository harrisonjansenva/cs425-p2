#include "lab.h"

uint16_t pkt_checksum(const uint8_t *data, size_t len)
{
   (void)data;
   (void)len;
   return 0;
}

size_t pkt_encode(const packet_t *pkt, uint8_t out[PKT_MAX_LEN])
{
   (void)pkt;
   (void)out;
   return 0;
}

pkt_status_t pkt_decode(const uint8_t *in, size_t in_len, packet_t *out)
{
   (void)in;
   (void)in_len;
   (void)out;
   return PKT_ERR_TOO_SHORT;
}
