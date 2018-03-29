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
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <time.h>

#include "common.h"


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
      /* CSV format: type (Answer), timestamp at the time of reception
	 (answer), connection ID, query ID, unused, unused, computed RTT in µs */
      printf("A,%lu.%.9lu,%u,%u,,,%lu\n",
	     now_realtime.tv_sec, now_realtime.tv_nsec,
	     params->connection_id,
	     query_id,
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
  static struct timespec now_realtime;
  struct tcp_connection *connection;
  struct poisson_process *params = ctx;
  /* Schedule next query */
  generate_poisson_interarrival(&interval, poisson_rate);
  int ret = event_add(params->write_event, &interval);
  if (ret != 0) {
    fprintf(stderr, "Failed to schedule next query (Poisson process %u)\n", params->process_id);
  }
  /* Select a TCP connection uniformly at random and send a query on it. */
  connection = &params->connections[lrand48() % nb_conn];
  if (print_rtt) {
    clock_gettime(CLOCK_REALTIME, &now_realtime);
    /* CSV format: type (Query), timestamp, connection ID, query ID, Poisson ID, poisson interval (in µs), unused. */
    printf("Q,%lu.%.9lu,%u,%u,%u,%lu,\n",
	   now_realtime.tv_sec, now_realtime.tv_nsec,
	   connection->connection_id,
	   connection->query_id,
	   params->process_id,
	   (1000000 * interval.tv_sec) + interval.tv_usec);
  }
  send_query(connection);
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
  struct event_config *ev_cfg;
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
  int bufev_fd;
  int on = 1;
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

  /* Compute maximum number of queries in flight.  Use a "safety factor"
     of 8 to account for the worst case. */
  double in_flight = 8 * (double) MAX_RTT_MSEC * (double) global_query_rate / (double) nb_conn / 1000.;
  if (in_flight > 65534) {
    max_queries_in_flight = 65535;
  } else if (in_flight < 20) {
    /* Use a minimum value to account for the worst case (burstiness on a
       single TCP connection) */
    max_queries_in_flight = 20;
  } else {
    max_queries_in_flight = ceil(in_flight);
  }
  debug("max queries in flight (per conn): %hu\n", max_queries_in_flight);

  /* How many Poisson processes do we need. */
  nb_poisson_processes = POISSON_PROCESS_PERIOD_MSEC * global_query_rate / 1000;
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

  if (print_rtt) {
    printf("type,timestamp,connection_id,query_id,poisson_id,poisson_interval_us,rtt_us\n");
  }

  /* Connect to server */
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

  /* Create event base with custom options */
  ev_cfg = event_config_new();
  if (!ev_cfg) {
    fprintf(stderr, "Couldn't allocate event base config\n");
    return 1;
  }
  int flags = 0;
  /* Small performance boost: locks are useless since we are not
     multi-threaded. */
  flags |= EVENT_BASE_FLAG_NOLOCK;
  /* epoll performance improvement */
  flags |= EVENT_BASE_FLAG_EPOLL_USE_CHANGELIST;
  /* Prevent libevent from using CLOCK_MONOTONIC_COARSE (introduced in
     libevent 2.1.5) */
#if LIBEVENT_VERSION_NUMBER >= 0x02010500
    flags |= EVENT_BASE_FLAG_PRECISE_TIMER;
#else
    info("Warning: libevent before 2.1.5 has very low timer resolution (1 ms)\n");
    info("Warning: You will likely obtain bursty request patterns\n");
#endif
  event_config_set_flag(ev_cfg, flags);
  base = event_base_new_with_config(ev_cfg);
  event_config_free(ev_cfg);
  if (!base) {
    fprintf(stderr, "Couldn't create event base\n");
    return 1;
  }

  /* Connect again, but using libevent, and multiple times. */
  info("Opening %u connections to host %s port %s...\n", nb_conn, host_s, port_s);
  bufevents = malloc(nb_conn * sizeof(struct bufferevent*));
  connections = malloc(nb_conn * sizeof(struct tcp_connection));
  for (conn_id = 0; conn_id < nb_conn; conn_id++) {
    errno = 0;
    /* Create and connect socket */
    sock = socket(server->ss_family, SOCK_STREAM, 0);
    if (sock == -1) {
      perror("Failed to create socket");
      break;
    }
    ret = connect(sock, (struct sockaddr*)server, server_len);
    if (ret != 0) {
      perror("Failed to connect to host");
      break;
    }
    ret = evutil_make_socket_nonblocking(sock);
    if (ret != 0) {
      perror("Failed to set socket to non-blocking mode");
      break;
    }

    bufevents[conn_id] = bufferevent_socket_new(base, sock, 0);
    if (bufevents[conn_id] == NULL) {
      perror("Failed to create socket-based bufferevent");
      break;
    }
    /* Disable Nagle */
    bufev_fd = bufferevent_getfd(bufevents[conn_id]);
    if (bufev_fd == -1) {
      info("Failed to disable Nagle on connection %ld (can't get file descriptor)\n", conn_id);
    } else {
      setsockopt(bufev_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
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
    generate_poisson_interarrival(&initial_timeout, poisson_rate);
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
