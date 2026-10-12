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
