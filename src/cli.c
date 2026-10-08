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
   if (s == NULL || s[0] == '\0')
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
   if (s == NULL || s[0] == '\0')
   {
      return false;
   }
   char *endptr = NULL;
   double val = strtod(s, &endptr);
   if (endptr == s || *endptr != '\0')
   {
      return false;
   }
   if (val < 0.0 || val > 0.5)
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
         if (!parse_u32(optarg, 0, UINT32_MAX, &timeout_ms))
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
         if (!parse_u32(optarg, 0, 65535, &port_val))
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
