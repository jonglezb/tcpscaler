#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
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

struct callback_data {
  struct poisson_process* process;
  struct udp_connection* connections;
};

/* Array of all UDP connections */
struct udp_connection *connections;

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

static void send_query_callback(void *ctx)
{
  static struct timespec now_realtime;
  struct udp_connection *connection;
  struct callback_data *data = ctx;
  /* Select a UDP connection uniformly at random and send a query on it. */
  connection = &data->connections[lrand48() % nb_conn];
  if (print_rtt) {
    clock_gettime(CLOCK_REALTIME, &now_realtime);
    /* CSV format: type (Query), timestamp, connection ID, query ID, Poisson ID, poisson interval (in µs), unused. */
    printf("Q,%lu.%.9lu,%u,%u,%u,,\n",
	   now_realtime.tv_sec, now_realtime.tv_nsec,
	   connection->connection_id,
	   connection->query_id,
	   data->process->process_id);
  }
  send_query(connection);
}

static void add_poisson_sender()
{
  struct poisson_process *process = poisson_new(base);
  struct callback_data *callback_arg = malloc(sizeof(struct callback_data));
  callback_arg->process = process;
  callback_arg->connections = connections;
  poisson_set_callback(process, send_query_callback, callback_arg);
  poisson_set_rate(process, poisson_rate);
  int ret = poisson_start_process(process, NULL);
  if (ret != 0) {
    fprintf(stderr, "Failed to start Poisson process %u\n", process->process_id);
  }
}

void usage(char* progname) {
  fprintf(stderr, "usage: %s [-h] [-v] [-R] [-s random_seed] [-t duration]  [--stdin]  [--stdin-rateslope]  -p <port>  -r <rate>  -c <nb_conn>  <host>\n",
	  progname);
  fprintf(stderr, "Connects to the specified host and port, with the chosen number of UDP connections.\n");
  fprintf(stderr, "[rate] is the total number of writes per second towards the server, accross all UDP connections.\n");
  fprintf(stderr, "Each write is 31 bytes.\n");
  fprintf(stderr, "With option '-R', print RTT samples as CSV: connection ID, reception timestamp, RTT in microseconds.\n");
  fprintf(stderr, "With option '-t', only send queries for the given amount of seconds.\n");
  fprintf(stderr, "With option '--stdin', the program ignores 'rate' and 'duration' and expects them\n");
  fprintf(stderr, "to be given on stdin as a sequence of '<duration_ms> <rate>' lines, with a first line giving the number of subsequent lines.\n");
  fprintf(stderr, "With option '--stdin-rateslope', the program starts from 'rate' qps, and expects\n");
  fprintf(stderr, "a sequence of '<duration_ms> <slope>' lines to be given on stdin, where each\n");
  fprintf(stderr, "'slope' in qps/s indicates how much to increase or decrease the query rate. The first line\n");
  fprintf(stderr, "must give the number of subsequent lines.\n");
  fprintf(stderr, "Option '-s' allows to choose a random seed (unsigned int) to determine times of transmission.  By default, the seed is set to 42\n");
}

int main(int argc, char** argv)
{
  struct event_config *ev_cfg;
  struct event *conn_event;
  struct addrinfo hints;
  struct addrinfo *res_list, *res;
  struct sockaddr_storage *server;
  struct timeval initial_timeout;
  struct timeval duration_timeval;
  /* Optional stdin-based commands */
  unsigned int nb_commands;
  struct command *commands;
  struct rateslope_command *rateslope_commands;
  struct event *change_rate_ev;
  unsigned int min_query_rate = 0xffffffff;
  unsigned int max_query_rate = 0;
  /* Used to change the limit of open files */
  struct rlimit limit_openfiles;
  int server_len;
  int sock;
  int ret;
  int opt;
  unsigned long int duration = 0, random_seed = 42;
  unsigned long int conn_id;
  unsigned int nb_poisson_processes;
  struct poisson_process *process;
  struct callback_data *callback_arg;
  char *host = NULL, *port = NULL;
  char host_s[NI_MAXHOST];
  char port_s[NI_MAXSERV];

  verbose = 0;
  print_rtt = 0;

  /* Start with options */
  int option_index = -1;
  static struct option long_options[] = {
    {"stdin",            no_argument, NULL, 0},
    {"stdin-rateslope",  no_argument, NULL, 0},
    {NULL,    0,         NULL, 0}
  };
  while ((opt = getopt_long(argc, argv, "p:r:c:n:vRs:t:h", long_options, &option_index)) != -1) {
    switch (opt) {
    case 0: /* long option */
      if (option_index == 0) { /* --stdin */
	stdin_commands = 1;
      }
      if (option_index == 1) { /* --stdin-rateslope */
	stdin_rateslope_commands = 1;
      }
      break;
    case 'p': /* UDP port */
      port = optarg;
      break;
    case 'r': /* Sending rate */
      min_query_rate = strtoul(optarg, NULL, 10);
      max_query_rate = min_query_rate;
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

  if (optind >= argc || port == NULL || (max_query_rate == 0 && stdin_commands == 0) || nb_conn == 0) {
    fprintf(stderr, "Error: missing mandatory arguments\n");
    usage(argv[0]);
    return 1;
  }
  if (stdin_commands == 1 && (duration != 0 || max_query_rate != 0 || stdin_rateslope_commands != 0)) {
    fprintf(stderr, "Error: --stdin is not compatible with -t, -r, or --stdin-rateslope\n");
    usage(argv[0]);
    return 1;
  }
  if (stdin_rateslope_commands == 1 && (duration != 0 || stdin_commands != 0)) {
    fprintf(stderr, "Error: --stdin-rateslope is not compatible with -t or --stdin\n");
    usage(argv[0]);
    return 1;
  }
  host = argv[optind];

  if (stdin_commands == 1) {
    ret = read_nb_commands(&nb_commands);
    if (ret == -1)
      return ret;
    /* Read commands */
    commands = calloc(nb_commands, sizeof(struct command));
    ret = read_commands(commands, &min_query_rate, &max_query_rate, nb_commands);
    if (ret == -1)
      return ret;
    debug("Minimum query rate: %u\n", min_query_rate);
    debug("Maximum query rate: %u\n", max_query_rate);
  } else if (stdin_rateslope_commands == 1) {
    ret = read_nb_commands(&nb_commands);
    if (ret == -1)
      return ret;
    /* Read "rate slope change" commands */
    rateslope_commands = calloc(nb_commands, sizeof(struct rateslope_command));
    ret = read_rateslope_commands(rateslope_commands, nb_commands);
    if (ret == -1)
      return ret;
  }

  srand48(random_seed);

  /* Compute maximum number of queries in flight.  Use a "safety factor"
     of 8 to account for the worst case. */
  double in_flight = 8 * (double) MAX_RTT_MSEC * (double) max_query_rate / (double) nb_conn / 1000.;
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
  nb_poisson_processes = POISSON_PROCESS_PERIOD_MSEC * min_query_rate / 1000;
  debug("Will spawn %d independent Poisson processes\n", nb_poisson_processes);

  if (stdin_commands == 1) {
    /* Set initial rate for each Poisson process */
    poisson_rate = (double) commands[0].query_rate / (double) nb_poisson_processes;
    info("Initial Poisson rate: %f\n", poisson_rate);
  }

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
  for (int i = 0; i < nb_poisson_processes; i++) {
    generate_poisson_interarrival(&initial_timeout, poisson_rate);
    /* Add 5 seconds to avoid missing query deadline even before we start
       the event loop.  Without this, the first queries all go out at the
       same time, creating a large burst. */
    initial_timeout.tv_sec += 5;
    debug("initial timeout %ld s %ld us\n", initial_timeout.tv_sec, initial_timeout.tv_usec);
    process = poisson_new(base);
    callback_arg = malloc(sizeof(struct callback_data));
    callback_arg->process = process;
    callback_arg->connections = connections;
    poisson_set_callback(process, send_query_callback, callback_arg);
    poisson_set_rate(process, poisson_rate);
    ret = poisson_start_process(process, &initial_timeout);
    if (ret != 0) {
      fprintf(stderr, "Failed to start Poisson process %u\n", process->process_id);
    }
  }

  /* Schedule stop event. */
  if (duration > 0) {
    info("Scheduling stop event in %ld seconds.\n", duration);
    /* Account for the 5 seconds delay on all events */
    duration_timeval.tv_sec = 5 + duration;
    duration_timeval.tv_usec = 0;
    event_base_loopexit(base, &duration_timeval);
  }

  /* Schedule changes of query rate. */
  if (stdin_commands == 1) {
    debug("Scheduling query rate changes according to stdin commands.\n");
    /* Accounts for 5-seconds delay on all events */
    struct timeval delay_timeval = {5, 0};
    for (int i = 0; i < nb_commands; ++i) {
      change_rate_ev = event_new(base, -1, 0, change_query_rate, &commands[i].query_rate);
      event_add(change_rate_ev, &delay_timeval);
      timeval_add_ms(&delay_timeval, commands[i].duration_ms);
    }
    event_base_loopexit(base, &delay_timeval);
  }

  /* Schedule changes of query rate slope. */
  if (stdin_rateslope_commands == 1) {
    debug("Scheduling query rate slope changes according to stdin commands.\n");
    /* Accounts for 5-seconds delay on all events */
    struct timeval delay_timeval = {5, 0};
    struct event *change_rate_slope_ev;
    for (int i = 0; i < nb_commands; ++i) {
      change_rate_slope_ev = event_new(base, -1, 0, change_query_rate_slope, &rateslope_commands[i]);
      event_add(change_rate_slope_ev, &delay_timeval);
      timeval_add_ms(&delay_timeval, rateslope_commands[i].duration_ms);
    }
    event_base_loopexit(base, &delay_timeval);
  }

  info("Starting event loop\n");
  event_base_dispatch(base);

  /* Free all the things */
  if (stdin_commands == 1) {
    free(commands);
  }
  if (stdin_rateslope_commands == 1) {
    free(rateslope_commands);
  }
  for (conn_id = 0; conn_id < nb_conn; conn_id++) {
    if (connections[conn_id].event != NULL) {
      event_free(connections[conn_id].event);
    }
    if (connections[conn_id].query_timestamps != NULL) {
      free(connections[conn_id].query_timestamps);
    }
  }
  free(connections);
  poisson_destroy(1);
  event_base_free(base);
  return 0;
}
