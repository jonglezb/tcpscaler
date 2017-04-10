#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>

static short verbose;

#define info(...) \
            do { if (verbose >= 1) fprintf(stderr, __VA_ARGS__); } while (0)

#define debug(...) \
            do { if (verbose >= 2) fprintf(stderr, __VA_ARGS__); } while (0)

static void readcb(struct bufferevent *bev, void *ctx)
{
  struct evbuffer *input = bufferevent_get_input(bev);
  size_t len = evbuffer_get_length(input);
  /* Discard input */
  evbuffer_drain(input, len);
}

static void writecb(evutil_socket_t fd, short events, void *ctx)
{
  struct bufferevent *bev = ctx;
  struct evbuffer *output = bufferevent_get_output(bev);
  static char data[64] = {0};
  evbuffer_add(output, data, 64);
}

/* The following is used to setup the periodic sending function (writecb).
   The idea is to have a first, one-shot event (setup_writecb) that sets
   up the actual periodic write event (writecb).  This allows to have all
   TCP connections use the same interval, but with a different initial
   offset.  Otherwise, all TCP connections would be synchronised and send
   data simultaneously. */

struct writecb_setup {
  struct bufferevent *bev;
  struct timeval interval;
};

static void setup_writecb(evutil_socket_t fd, short events, void *ctx)
{
  struct writecb_setup *setup = ctx;
  struct event_base *base = bufferevent_get_base(setup->bev);
  struct event *writeev;
  int ret;
  /* Setup periodic task to send data */
  writeev = event_new(base, -1, EV_PERSIST, writecb, setup->bev);
  ret = event_add(writeev, &setup->interval);
  if (ret != 0) {
    fprintf(stderr, "Failed to add periodic sending task\n");
  }
  /* Also send the first data. */
  writecb(-1, 0, setup->bev);
}

static void eventcb(struct bufferevent *bev, short events, void *ptr)
{
  if (events & BEV_EVENT_ERROR) {
    perror("Connection error");
  }
}

void usage(char* progname) {
  fprintf(stderr, "usage: %s [-h] [-v]  -p <port>  -r <rate>  -t <nb_conn>  <host>\n",
	  progname);
  fprintf(stderr, "Connects to the specified host and port, with the chosen number of TCP connections.\n");
  fprintf(stderr, "[rate] is the total number of writes per second towards the server, accross all TCP connections.\n");
  fprintf(stderr, "Each write is 64 bytes.\n");
}

int main(int argc, char** argv)
{
  struct event_base *base;
  struct bufferevent **bufevents;
  struct addrinfo hints;
  struct addrinfo *res_list, *res;
  struct sockaddr_storage *server;
  struct event *setup_writeev;
  struct timeval write_interval;
  struct timeval initial_timeout;
  struct writecb_setup *setups;
  int server_len;
  int sock;
  int ret;
  int opt;
  unsigned long int nb_conn = 0, rate = 0;
  unsigned long int conn, rand_usec;
  char *host, *port;
  char host_s[24];
  char port_s[6];

  verbose = 0;

  /* Start with options */
  while ((opt = getopt(argc, argv, "p:r:t:vh")) != -1) {
    switch (opt) {
    case 'p': /* TCP port */
      port = optarg;
      break;
    case 'r': /* rate */
      rate = strtoul(optarg, NULL, 10);
      break;
    case 't': /* Number of TCP connections */
      nb_conn = strtoul(optarg, NULL, 10);
      break;
    case 'v': /* verbose */
      verbose += 1;
      break;
    case 'h': /* help */
      usage(argv[0]);
      return 0;
      break;
    default:
      usage(argv[0]);
      return 1;
    }
  }

  if (optind >= argc || port == NULL || rate == 0 || nb_conn == 0) {
    fprintf(stderr, "Error: missing mandatory arguments\n");
    usage(argv[0]);
    return 1;
  }
  host = argv[optind];
  /* Interval between two writes, for a single TCP connection. */
  write_interval.tv_sec = nb_conn / rate;
  write_interval.tv_usec = (1000000 * nb_conn / rate) % 1000000;
  write_interval.tv_usec = write_interval.tv_usec > 0 ? write_interval.tv_usec : 1;
  debug("write interval %ld s %ld us\n", write_interval.tv_sec, write_interval.tv_usec);

  srandom(42);

  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = 0;
  hints.ai_protocol = 0;

  ret = getaddrinfo(host, port, &hints, &res_list);
  if (ret != 0) {
    fprintf(stderr, "Error in getaddrinfo: %s\n", gai_strerror(ret));
    return 1;
  }

  for (res = res_list; res != NULL; res = res->ai_next) {
    sock = socket(res->ai_family, res->ai_socktype,
		  res->ai_protocol);
    if (sock == -1)
      continue;

    getnameinfo(res->ai_addr, res->ai_addrlen, host_s, 24,
		port_s, 6, NI_NUMERICHOST | NI_NUMERICSERV);
    info("Trying to connect to %s port %s...\n", host_s, port_s);
    if (connect(sock, res->ai_addr, res->ai_addrlen) != -1) {
      info("Success!\n");
      close(sock);
      break;
    } else {
      perror("Failed to connect");
      close(sock);
    }
  }

  /* No address succeeded */
  if (res == NULL) {
    fprintf(stderr, "Could not connect to host\n");
    return 1;
  }

  /* Copy working server */
  server = malloc(sizeof(struct sockaddr_storage));
  memcpy(server, res->ai_addr, sizeof(struct sockaddr_storage));
  server_len = res->ai_addrlen;
  freeaddrinfo(res_list);

  base = event_base_new();
  if (!base) {
    fprintf(stderr, "Couldn't open event base\n");
    return 1;
  }

  /* Connect again, but using libevent, and multiple times. */
  bufevents = malloc(nb_conn * sizeof(struct bufferevent*));
  setups = malloc(nb_conn * sizeof(struct writecb_setup));
  for (conn = 0; conn < nb_conn; conn++) {
    bufevents[conn] = bufferevent_socket_new(base, -1, 0);
    if (bufevents[conn] == NULL) {
      fprintf(stderr, "Failed to create socket-based bufferevent.\n");
      break;
    }
    ret = bufferevent_socket_connect(bufevents[conn], (struct sockaddr*)server, server_len);
    if (ret != 0) {
      fprintf(stderr, "Failed to connect to host with bufferevent.\n");
      bufferevent_free(bufevents[conn]);
      bufevents[conn] = NULL;
      break;
    }
    bufferevent_setcb(bufevents[conn], readcb, NULL, eventcb, NULL);
    bufferevent_enable(bufevents[conn], EV_READ|EV_WRITE);
    /* Schedule task setup_writecb with a random offset. */
    rand_usec = random() % (1000000 * write_interval.tv_sec + write_interval.tv_usec + 1);
    initial_timeout.tv_sec = rand_usec / 1000000;
    initial_timeout.tv_usec = rand_usec % 1000000;
    debug("initial timeout %ld s %ld us\n", initial_timeout.tv_sec, initial_timeout.tv_usec);
    setups[conn].interval = write_interval;
    setups[conn].bev = bufevents[conn];
    setup_writeev = event_new(base, -1, 0, setup_writecb, &(setups[conn]));
    ret = event_add(setup_writeev, &initial_timeout);
    if (ret != 0) {
      fprintf(stderr, "Failed to add periodic sending task for connection %ld\n", conn);
    }
    /* Progress output */
    if (conn % 500 == 0)
      info("Opened %ld connections so far...\n", conn);

    /* Wait a bit, 1ms means 1000 new connections each second. */
    usleep(1000);
  }
  info("Opened %ld connections to host %s port %s\n", conn, host_s, port_s);

  printf("Starting event loop\n");
  event_base_dispatch(base);

  /* Free all the things */
  for (conn = 0; conn < nb_conn; conn++) {
    if (bufevents[conn] == NULL)
      break;
    bufferevent_free(bufevents[conn]);
  }
  free(bufevents);
  free(setups);
  event_base_free(base);
  return 0;
}
