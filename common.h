#include "utils.h"

/* Maximum expected response time for a query.  This is used to compute
   how many queries in flight we should expect on each connection, and
   thus how many timestamps to keep in memory. */
#define MAX_RTT_MSEC 60000

/* Average sending period of a single Poisson process (in milliseconds per
   query).  We will spawn as much iid Poisson processes as needed to
   achieve the target query rate.  Each Poisson process should be slow
   enough so that the scheduling overhead has minimal impact.

   Note that when variable query rate is used (stdin commands), this value
   is only valid for the slowest query rate (i.e. for higher query rates,
   the Poisson period will be lower). */
#define POISSON_PROCESS_PERIOD_MSEC 1000

/* How many commands we are prepared to accept on stdin. */
#define MAX_STDIN_COMMANDS 256



static short verbose;
static short print_rtt;
/* Whether we take commands from stdin (sequence of duration and query rate) */
static short stdin_commands;
/* Maximum number of queries "in flight" on a given UDP or TCP connection.
   Computed from MAX_RTT, rate, and nb_conn. */
static uint16_t max_queries_in_flight;
/* How many Poisson processes we are running in parallel, and at which rate. */
static uint32_t nb_poisson_processes = 0;
static double poisson_rate = 1000. / (double) POISSON_PROCESS_PERIOD_MSEC;
/* How many UDP or TCP connections we maintain. */
static uint32_t nb_conn = 0;


struct command {
  unsigned int duration_ms;
  unsigned int query_rate;
};


int read_nb_commands(unsigned int *nb_commands)
{
  int ret = scanf("%u", nb_commands);
  if (ret != 1) {
    fprintf(stderr, "Error: expected number of commands on first line of stdin\n");
    return -1;
  }
  if (*nb_commands > MAX_STDIN_COMMANDS) {
    fprintf(stderr, "Error: maximum number of allowed commands is %u\n", MAX_STDIN_COMMANDS);
    return -1;
  }
  if (*nb_commands == 0) {
    fprintf(stderr, "Error: at least one command expected\n");
    return -1;
  }
  return ret;
}

int read_commands(struct command *commands, unsigned int *min_rate, unsigned int *max_rate, unsigned int nb_commands)
{
  int ret = 2;
  *min_rate = 0xffffffff;
  *max_rate = 0;
  for (int i = 0; i < nb_commands; ++i) {
    ret = scanf("%u %u", &commands[i].duration_ms, &commands[i].query_rate);
    if (ret != 2) {
      fprintf(stderr, "Error parsing command input\n");
      return -1;
    }
    /* Find smallest and largest query rate */
    if (commands[i].query_rate > *max_rate)
      *max_rate = commands[i].query_rate;
    if (commands[i].query_rate < *min_rate)
      *min_rate = commands[i].query_rate;
  }
  return 0;
}

static void change_query_rate(evutil_socket_t fd, short events, void *ctx)
{
  unsigned int *new_rate = ctx;
  poisson_rate = (double) *new_rate / (double) nb_poisson_processes;
  info("Changed Poisson rate to %f\n", poisson_rate);
}

/* Given a [rate], generate an interarrival sample according to a Poisson
   process and store it in [tv]. */
void generate_poisson_interarrival(struct timeval* tv, double rate)
{
  double u = drand48();
  double interarrival = - log(1. - u) / rate;
  tv->tv_sec = (time_t) floor(interarrival);
  tv->tv_usec = lrint(interarrival * 1000000.) % 1000000;
}
