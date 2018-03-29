#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <math.h>
#include <arpa/inet.h>
#include <event2/event.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <time.h>

#include "common.h"


struct udp_connection {
  /* Event associated with this connection. */
  struct event* event;
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
  /* Array of all UDP connections. */
  struct udp_connection* connections;
};

static void ev_callback(evutil_socket_t fd, short events, void *ctx)
{
  static char buf[256];
  if ((events & EV_READ) == 0) {
    info("Warning: unexpected event on connection callback\n");
    return;
  }
  struct udp_connection *conn = ctx;
  evutil_socket_t sock;
  ssize_t ret;
  uint16_t query_id;
  struct timespec* query_timestamp;
  struct timespec now, rtt;
  /* Used for logging, because "now" uses a monotonic clock. */
  struct timespec now_realtime;
  if (!print_rtt) {
    /* Just discard the message to avoid filling OS buffer. */
    sock = event_get_fd(conn->event);
    read(sock, buf, sizeof(buf));
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &now);
  clock_gettime(CLOCK_REALTIME, &now_realtime);
  sock = event_get_fd(conn->event);
  ret = read(sock, buf, sizeof(buf));
  if (ret == -1 || ret < 2) {
    return;
  }
  /* Extract query ID in the answer (assuming it is either a real DNS
     answer, or just our query being reflected back to us). */
  DO_NTOHS(query_id, buf);
  /* Compute RTT, in microseconds */
  query_timestamp = &conn->query_timestamps[query_id % max_queries_in_flight];
  subtract_timespec(&rtt, &now, query_timestamp);
  /* CSV format: type (Answer), timestamp at the time of reception
     (answer), connection ID, query ID, unused, unused, computed RTT in µs */
  printf("A,%lu.%.9lu,%u,%u,,,%lu\n",
	 now_realtime.tv_sec, now_realtime.tv_nsec,
	 conn->connection_id,
	 query_id,
	 (rtt.tv_nsec / 1000) + (1000000 * rtt.tv_sec));
}

static void send_query(struct udp_connection* conn)
{
  /* DNS query for example.com (with type A) */
  static char data[] = {
    0xff, 0xff, /* Query ID */
    0x01, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x07, 0x65, 0x78, 0x61,
    0x6d, 0x70, 0x6c, 0x65, 0x03, 0x63, 0x6f, 0x6d,
    0x00, 0x00, 0x01, 0x00, 0x01
  };
  ssize_t ret;
  evutil_socket_t sock = event_get_fd(conn->event);
  /* Copy query ID */
  DO_HTONS(data, conn->query_id);
  /* Record timestamp */
  clock_gettime(CLOCK_MONOTONIC, &conn->query_timestamps[conn->query_id % max_queries_in_flight]);
  ret = send(sock, data, sizeof(data), 0);
  if (ret == -1) {
    perror("Error sending query");
  }
  conn->query_id += 1;
}

static void poisson_process_writecb(evutil_socket_t fd, short events, void *ctx)
{
  static struct timeval interval;
  static struct timespec now_realtime;
  struct udp_connection *connection;
  struct poisson_process *params = ctx;
  /* Schedule next query */
  generate_poisson_interarrival(&interval, poisson_rate);
  int ret = event_add(params->write_event, &interval);
  if (ret != 0) {
    fprintf(stderr, "Failed to schedule next query (Poisson process %u)\n", params->process_id);
  }
  /* Select a UDP connection uniformly at random and send a query on it. */
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

void usage(char* progname) {
  fprintf(stderr, "usage: %s [-h] [-v] [-R] [-s random_seed] [-t duration]  -p <port>  -r <rate>  -c <nb_conn>  <host>\n",
	  progname);
  fprintf(stderr, "Connects to the specified host and port, with the chosen number of UDP connections.\n");
  fprintf(stderr, "[rate] is the total number of writes per second towards the server, accross all UDP connections.\n");
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
  struct event *conn_event;
  struct addrinfo hints;
  struct addrinfo *res_list, *res;
  struct sockaddr_storage *server;
  struct timeval initial_timeout;
  struct timeval duration_timeval;
  /* Array of all UDP connections */
  struct udp_connection *connections;
  /* Array of all Poisson processes */
  struct poisson_process *processes;
  struct rlimit limit_openfiles;
  int server_len;
  int sock;
  int ret;
  int opt;
  unsigned long int global_query_rate = 0, duration = 0, random_seed = 42;
  unsigned long int conn_id, process_id;
  char *host = NULL, *port = NULL;
  char host_s[NI_MAXHOST];
  char port_s[NI_MAXSERV];

  verbose = 0;
  print_rtt = 0;

  /* Start with options */
  while ((opt = getopt(argc, argv, "p:r:c:n:vRs:t:h")) != -1) {
    switch (opt) {
    case 'p': /* UDP port */
      port = optarg;
      break;
    case 'r': /* Sending rate */
      global_query_rate = strtoul(optarg, NULL, 10);
      break;
    case 'c': /* Number of UDP connections */
      nb_conn = strtoul(optarg, NULL, 10);
      break;
    case 'n': /* Rate of new connections (#/sec) */
      info("Warning: option -n ignored for UDP\n");
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
       single UDP connection) */
    max_queries_in_flight = 20;
  } else {
    max_queries_in_flight = ceil(in_flight);
  }
  debug("max queries in flight (per conn): %hu\n", max_queries_in_flight);

  /* How many Poisson processes do we need. */
  nb_poisson_processes = POISSON_PROCESS_PERIOD_MSEC * global_query_rate / 1000;
  debug("Will spawn %d independent Poisson processes\n", nb_poisson_processes);

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
  info("Maximum number of UDP connections: %ld\n", limit_openfiles.rlim_cur);
  if (nb_conn > limit_openfiles.rlim_cur) {
    fprintf(stderr,
	    "Warning: requested number of UDP connections (%u) larger then maximum number of open files (%ld)\n",
	    nb_conn, limit_openfiles.rlim_cur);
  }

  if (print_rtt) {
    printf("type,timestamp,connection_id,query_id,poisson_id,poisson_interval_us,rtt_us\n");
  }

  /* Connect to server */
  memset(&hints, 0, sizeof(struct addrinfo));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_DGRAM;
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
  connections = malloc(nb_conn * sizeof(struct udp_connection));
  for (conn_id = 0; conn_id < nb_conn; conn_id++) {
    errno = 0;
    /* Create and connect socket */
    sock = socket(server->ss_family, SOCK_DGRAM, 0);
    if (sock == -1) {
      perror("Failed to create socket");
      break;
    }
    ret = connect(sock, (struct sockaddr*)server, server_len);
    if (ret != 0) {
      perror("Failed to connect to host");
      break;
    }
    /* Create connection and associated event */
    conn_event = event_new(base, sock, EV_READ|EV_PERSIST, ev_callback, &connections[conn_id]);
    if (conn_event == NULL) {
      fprintf(stderr, "Failed to create UDP event\n");
      break;
    }
    connections[conn_id].event = conn_event;
    connections[conn_id].connection_id = conn_id;
    connections[conn_id].query_id = 0;
    connections[conn_id].query_timestamps = malloc(max_queries_in_flight * sizeof(struct timespec));
    event_add(conn_event, NULL);
  }
  info("Opened %ld connections to host %s port %s\n", conn_id, host_s, port_s);

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
    if (connections[conn_id].event != NULL) {
      event_free(connections[conn_id].event);
    }
    if (connections[conn_id].query_timestamps != NULL) {
      free(connections[conn_id].query_timestamps);
    }
  }
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
