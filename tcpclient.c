#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <arpa/inet.h>
#include <event2/event.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netdb.h>
#include <time.h>

/* Maximum expected response time for a query.  This is used to compute
   how many queries in flight we should expect on each connection, and
   thus how many timestamps to keep in memory. */
#define MAX_RTT_MSEC 60000

/* Average sending period of a single Poisson process (in milliseconds per
   query).  We will spawn as much iid Poisson processes as needed to
   achieve the target query rate.  Each Poisson process should be slow
   enough so that the scheduling overhead has minimal impact. */
#define POISSON_PROCESS_PERIOD_MSEC 1000

/* Copied from babeld by Juliusz Chroboczek */
#define DO_NTOHS(_d, _s) \
    do { unsigned short _dd; \
         memcpy(&(_dd), (_s), 2); \
         _d = ntohs(_dd); } while(0)
#define DO_NTOHL(_d, _s) \
    do { unsigned int _dd; \
         memcpy(&(_dd), (_s), 4); \
         _d = ntohl(_dd); } while(0)
#define DO_HTONS(_d, _s) \
    do { unsigned short _dd; \
         _dd = htons(_s); \
         memcpy((_d), &(_dd), 2); } while(0)
#define DO_HTONL(_d, _s) \
    do { unsigned _dd; \
         _dd = htonl(_s); \
         memcpy((_d), &(_dd), 4); } while(0)

#define error(...) \
            do { fprintf(stderr, __VA_ARGS__); } while (0)

#define info(...) \
            do { if (verbose >= 1) fprintf(stderr, __VA_ARGS__); } while (0)

#define debug(...) \
            do { if (verbose >= 2) fprintf(stderr, __VA_ARGS__); } while (0)


static short verbose;
static short print_rtt;
/* Maximum number of queries "in flight" on a given TCP connection.
   Computed from MAX_RTT, rate, and nb_conn. */
static uint16_t max_queries_in_flight;
/* How many Poisson processes we are running in parallel, and at which rate. */
static uint32_t nb_poisson_processes = 0;
static double poisson_rate = 1000. / (double) POISSON_PROCESS_PERIOD_MSEC;
/* How many TCP connections we maintain. */
static uint32_t nb_conn = 0;


struct tcp_connection {
  /* The actual connection, encapsulated in a bufferevent. */
  struct bufferevent *bev;
  /* ID of the connection, mostly for logging purpose. */
  uint32_t connection_id;
  /* Current query ID, incremented for each query and used to index the
     query_timestamps array. */
  uint16_t query_id;
  /* Used to remember when we sent the last [max_queries_in_flight]
     queries, to compute a RTT. */
  struct timespec* query_timestamps;
};

struct poisson_process {
  /* ID of the Poisson process, mostly for logging purpose. */
  uint32_t process_id;
  /* Used to schedule the next event for this Poisson process. */
  struct event* write_event;
  /* Array of all TCP connections. */
  struct tcp_connection* connections;
};

/* Given a [rate], generate an interarrival sample according to a Poisson
   process and store it in [tv]. */
void generate_poisson_interarrival(double rate, struct timeval* tv)
{
  double u = drand48();
  double interarrival = - log(1. - u) / rate;
  tv->tv_sec = (time_t) floor(interarrival);
  tv->tv_usec = lrint(interarrival * 1000000.) % 1000000;
}

void subtract_timespec(struct timespec *result, const struct timespec *a, const struct timespec *b)
{
  if ((a->tv_sec < b->tv_sec) ||
      ((a->tv_sec == b->tv_sec) &&
       (a->tv_nsec <= b->tv_nsec))) {		/* TIME1 <= TIME2? */
    result->tv_sec = result->tv_nsec = 0 ;
  } else {						/* TIME1 > TIME2 */
    result->tv_sec = a->tv_sec - b->tv_sec ;
    if (a->tv_nsec < b->tv_nsec) {
      result->tv_nsec = a->tv_nsec + 1000000000L - b->tv_nsec ;
      result->tv_sec-- ;				/* Borrow a second. */
    } else {
      result->tv_nsec = a->tv_nsec - b->tv_nsec ;
    }
  }
}

static void readcb(struct bufferevent *bev, void *ctx)
{
  struct tcp_connection *params = ctx;
  unsigned char* input_ptr;
  uint16_t dns_len;
  uint16_t query_id;
  struct timespec* query_timestamp;
  struct timespec now, rtt;
  /* Used for logging, because "now" uses a monotonic clock. */
  struct timespec now_realtime;
  /* Retrieve response (or mirrored message), and make sure it is a
     complete DNS message.  We retrieve the query ID to compute the
     RTT. */
  struct evbuffer *input = bufferevent_get_input(bev);
  debug("Entering readcb\n");
  /* Loop until we cannot read a complete DNS message. */
  while (1) {
    if (print_rtt) {
      clock_gettime(CLOCK_MONOTONIC, &now);
      clock_gettime(CLOCK_REALTIME, &now_realtime);
    }
    size_t input_len = evbuffer_get_length(input);
    if (input_len < 4) {
      if (input_len > 0) {
	debug("Short read with size %lu, aborting for now\n", input_len);
      }
      return;
    }
    input_ptr = evbuffer_pullup(input, 4);
    DO_NTOHS(dns_len, input_ptr);
    DO_NTOHS(query_id, input_ptr + 2);
    debug("Input buffer length: %lu ; DNS length: %hu ; Query ID: %hu\n",
	  input_len, dns_len, query_id);
    if (input_len < dns_len + 2) {
      /* Incomplete message */
      debug("Incomplete DNS reply for query ID %hu (%lu bytes out of %hu), aborting for now\n",
	    query_id, input_len - 2, dns_len);
      return;
    }
    /* We are now certain to have a complete DNS message. */
    /* Compute RTT, in microseconds */
    if (print_rtt) {
      query_timestamp = &params->query_timestamps[query_id % max_queries_in_flight];
      subtract_timespec(&rtt, &now, query_timestamp);
      /* CSV format: connection ID, timestamp at the time of reception
	 (answer), computed RTT in µs */
      printf("%u,%lu.%.9lu,%lu\n",
	     params->connection_id,
	     now_realtime.tv_sec, now_realtime.tv_nsec,
	     (rtt.tv_nsec / 1000) + (1000000 * rtt.tv_sec));
    }
    /* Discard the DNS message (including the 2-bytes length prefix) */
    evbuffer_drain(input, dns_len + 2);
  }
}

static void send_query(struct tcp_connection* conn)
{
  /* DNS query for example.com (with type A) */
  static char data[] = {
    0x00, 0x1d, /* Size */
    0xff, 0xff, /* Query ID */
    0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x07, 0x65, 0x78, 0x61,
    0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d,
    0x00, 0x00, 0x01, 0x00, 0x01
  };
  struct bufferevent *bev = conn->bev;
  struct evbuffer *output = bufferevent_get_output(bev);
  /* Copy query ID */
  DO_HTONS(data + 2, conn->query_id);
  /* Record timestamp */
  clock_gettime(CLOCK_MONOTONIC, &conn->query_timestamps[conn->query_id % max_queries_in_flight]);
  evbuffer_add(output, data, sizeof(data));
  conn->query_id += 1;
}

static void poisson_process_writecb(evutil_socket_t fd, short events, void *ctx)
{
  static struct timeval interval;
  struct poisson_process *params = ctx;
  /* Schedule next query */
  generate_poisson_interarrival(poisson_rate, &interval);
  int ret = event_add(params->write_event, &interval);
  if (ret != 0) {
    fprintf(stderr, "Failed to schedule next query (Poisson process %u)\n", params->process_id);
  }
  /* Select a TCP connection uniformly at random and send a query on it. */
  send_query(&params->connections[lrand48() % nb_conn]);
}

static void eventcb(struct bufferevent *bev, short events, void *ptr)
{
  if (events & BEV_EVENT_ERROR) {
    perror("Connection error");
  }
}

void usage(char* progname) {
  fprintf(stderr, "usage: %s [-h] [-v] [-R] [-s random_seed] [-t duration]  [-n new_conn_rate]  -p <port>  -r <rate>  -c <nb_conn>  <host>\n",
	  progname);
  fprintf(stderr, "Connects to the specified host and port, with the chosen number of TCP connections.\n");
  fprintf(stderr, "[rate] is the total number of writes per second towards the server, accross all TCP connections.\n");
  fprintf(stderr, "Each write is 31 bytes.\n");
  fprintf(stderr, "[new_conn_rate] is the number of new connections to open per second when starting the client.\n");
  fprintf(stderr, "With option '-R', print RTT samples as CSV: connection ID, reception timestamp, RTT in microseconds.\n");
  fprintf(stderr, "With option '-t', only send queries for the given amount of seconds.\n");
  fprintf(stderr, "Option '-s' allows to choose a random seed (unsigned int) to determine times of transmission.  By default, the seed is set to 42\n");
}

int main(int argc, char** argv)
{
  struct event_base *base;
  struct bufferevent **bufevents;
  struct addrinfo hints;
  struct addrinfo *res_list, *res;
  struct sockaddr_storage *server;
  struct timeval initial_timeout;
  struct timeval duration_timeval;
  /* Array of all TCP connections */
  struct tcp_connection *connections;
  /* Array of all Poisson processes */
  struct poisson_process *processes;
  struct rlimit limit_openfiles;
  int server_len;
  int sock;
  int ret;
  int opt;
  unsigned long int global_query_rate = 0, duration = 0, new_conn_rate = 1000, random_seed = 42;
  unsigned long int new_conn_interval;
  unsigned long int conn_id, process_id;
  char *host = NULL, *port = NULL;
  char host_s[NI_MAXHOST];
  char port_s[NI_MAXSERV];

  verbose = 0;
  print_rtt = 0;

  /* Start with options */
  while ((opt = getopt(argc, argv, "p:r:c:n:vRs:t:h")) != -1) {
    switch (opt) {
    case 'p': /* TCP port */
      port = optarg;
      break;
    case 'r': /* Sending rate */
      global_query_rate = strtoul(optarg, NULL, 10);
      break;
    case 'c': /* Number of TCP connections */
      nb_conn = strtoul(optarg, NULL, 10);
      break;
    case 'n': /* Rate of new connections (#/sec) */
      new_conn_rate = strtoul(optarg, NULL, 10);
      break;
    case 'v': /* verbose */
      verbose += 1;
      break;
    case 'R': /* Print RTT */
      print_rtt = 1;
      break;
    case 's': /* Random seed */
      random_seed = strtoul(optarg, NULL, 10);
      break;
    case 't': /* Duration */
      duration = strtoul(optarg, NULL, 10);
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

  if (optind >= argc || port == NULL || global_query_rate == 0 || nb_conn == 0) {
    fprintf(stderr, "Error: missing mandatory arguments\n");
    usage(argv[0]);
    return 1;
  }
  host = argv[optind];

  srand48(random_seed);

  /* Compute maximum number of queries in flight. */
  double in_flight = (double) MAX_RTT_MSEC * (double) global_query_rate / (double) nb_conn / 1000.;
  if (in_flight > 65534) {
    max_queries_in_flight = 65535;
  } else {
    max_queries_in_flight = ceil(in_flight);
  }
  debug("max queries in flight (per conn): %hu\n", max_queries_in_flight);

  /* How many Poisson processes do we need. */
  nb_poisson_processes = 1000 * global_query_rate / POISSON_PROCESS_PERIOD_MSEC;
  debug("Will spawn %d independent Poisson processes\n", nb_poisson_processes);

  /* Interval between two new connections, in microseconds. */
  new_conn_interval = 1000000 / new_conn_rate;

  /* Set maximum number of open files (set soft limit to hard limit) */
  ret = getrlimit(RLIMIT_NOFILE, &limit_openfiles);
  if (ret != 0) {
    perror("Failed to get limit on number of open files");
  }
  limit_openfiles.rlim_cur = limit_openfiles.rlim_max;
  ret = setrlimit(RLIMIT_NOFILE, &limit_openfiles);
  if (ret != 0) {
    perror("Failed to set limit on number of open files");
  }
  info("Maximum number of TCP connections: %ld\n", limit_openfiles.rlim_cur);
  if (nb_conn > limit_openfiles.rlim_cur) {
    fprintf(stderr,
	    "Warning: requested number of TCP connections (%u) larger then maximum number of open files (%ld)\n",
	    nb_conn, limit_openfiles.rlim_cur);
  }

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

    getnameinfo(res->ai_addr, res->ai_addrlen, host_s, NI_MAXHOST,
		port_s, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);
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
  info("Opening %u connections to host %s port %s...\n", nb_conn, host_s, port_s);
  bufevents = malloc(nb_conn * sizeof(struct bufferevent*));
  connections = malloc(nb_conn * sizeof(struct tcp_connection));
  for (conn_id = 0; conn_id < nb_conn; conn_id++) {
    errno = 0;
    bufevents[conn_id] = bufferevent_socket_new(base, -1, 0);
    if (bufevents[conn_id] == NULL) {
      perror("Failed to create socket-based bufferevent");
      break;
    }
    errno = 0;
    ret = bufferevent_socket_connect(bufevents[conn_id], (struct sockaddr*)server, server_len);
    if (ret != 0) {
      perror("Failed to connect to host with bufferevent");
      bufferevent_free(bufevents[conn_id]);
      bufevents[conn_id] = NULL;
      break;
    }
    connections[conn_id].connection_id = conn_id;
    connections[conn_id].query_id = 0;
    connections[conn_id].bev = bufevents[conn_id];
    connections[conn_id].query_timestamps = malloc(max_queries_in_flight * sizeof(struct timespec));
    bufferevent_setcb(bufevents[conn_id], readcb, NULL, eventcb, &connections[conn_id]);
    bufferevent_enable(bufevents[conn_id], EV_READ|EV_WRITE);

    /* Progress output, roughly once per second */
    if (conn_id % new_conn_rate == 0)
      debug("Opened %ld connections so far...\n", conn_id);

    /* Wait a bit between each connection to avoid overwhelming the server. */
    usleep(new_conn_interval);
  }
  info("Opened %ld connections to host %s port %s\n", conn_id, host_s, port_s);

  /* Leave some time for all connections to connect */
  sleep(3 + nb_conn / 5000);

  info("Starting %u Poisson processes generating queries...\n", nb_poisson_processes);
  processes = malloc(nb_poisson_processes * sizeof(struct poisson_process));
  for (process_id = 0; process_id < nb_poisson_processes; process_id++) {
    generate_poisson_interarrival(poisson_rate, &initial_timeout);
    /* Add 5 seconds to avoid missing query deadline even before we start
       the event loop.  Without this, the first queries all go out at the
       same time, creating a large burst. */
    initial_timeout.tv_sec += 5;
    debug("initial timeout %ld s %ld us\n", initial_timeout.tv_sec, initial_timeout.tv_usec);
    processes[process_id].process_id = process_id;
    processes[process_id].connections = connections;
    processes[process_id].write_event = event_new(base, -1, 0, poisson_process_writecb, &(processes[process_id]));
    ret = event_add(processes[process_id].write_event, &initial_timeout);
    if (ret != 0) {
      fprintf(stderr, "Failed to start Poisson process %ld\n", process_id);
    }
  }

  /* Schedule stop event. */
  if (duration > 0) {
    info("Scheduling stop event in %ld seconds.\n", duration);
    duration_timeval.tv_sec = duration;
    duration_timeval.tv_usec = 0;
    event_base_loopexit(base, &duration_timeval);
  }

  info("Starting event loop\n");
  event_base_dispatch(base);

  /* Free all the things */
  for (conn_id = 0; conn_id < nb_conn; conn_id++) {
    if (bufevents[conn_id] == NULL)
      break;
    bufferevent_free(bufevents[conn_id]);
    if (connections[conn_id].query_timestamps != NULL) {
      free(connections[conn_id].query_timestamps);
    }
  }
  free(bufevents);
  free(connections);
  for (process_id = 0; process_id < nb_poisson_processes; process_id++) {
    if (processes[process_id].write_event != NULL) {
      event_free(processes[process_id].write_event);
    }
  }
  free(processes);
  event_base_free(base);
  return 0;
}
