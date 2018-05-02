#include "poisson.h"
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

/* Interval between updates of query rate slope, when --stdin-rateslope is
   used.  The actual interval might be slightly different to ensure that
   the target rate slope is achieved, because we can only add/remove an
   integral number of poisson process at each update. */
#define RATE_SLOPE_UPDATE_INTERVAL_MSEC 100

/* How many commands we are prepared to accept on stdin. */
#define MAX_STDIN_COMMANDS 256



struct event_base *base;
static short verbose;
static short print_rtt;
/* Whether we take commands from stdin (sequence of duration and query rate) */
static short stdin_commands;
/* Whether we take slope commands from stdin (sequence of duration and query rate slope) */
static short stdin_rateslope_commands;
/* Maximum number of queries "in flight" on a given UDP or TCP connection.
   Computed from MAX_RTT, rate, and nb_conn. */
static uint16_t max_queries_in_flight;
/* Sending rate of each Poisson process. */
static double poisson_rate = 1000. / (double) POISSON_PROCESS_PERIOD_MSEC;
/* How many UDP or TCP connections we maintain. */
static uint32_t nb_conn = 0;


struct command {
  unsigned int duration_ms;
  unsigned int query_rate;
};

struct rateslope_command {
  unsigned int duration_ms;
  /* Slope of the query rate increase/decrease, in qps per second */
  int query_rate_slope;
};

static void add_poisson_sender();

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

int read_rateslope_commands(struct rateslope_command *commands, unsigned int nb_commands)
{
  int ret = 2;
  for (int i = 0; i < nb_commands; ++i) {
    ret = scanf("%u %d", &commands[i].duration_ms, &commands[i].query_rate_slope);
    if (ret != 2) {
      fprintf(stderr, "Error parsing command input\n");
      return -1;
    }
  }
  return 0;
}

static void change_query_rate(evutil_socket_t fd, short events, void *ctx)
{
  unsigned int *new_rate = ctx;
  poisson_rate = (double) *new_rate / (double) poisson_nb_processes();
  /* TODO: apply this rate to all processes. */
  info("Changed Poisson rate to %f\n", poisson_rate);
}

static void stop_event(evutil_socket_t fd, short events, void *ctx)
{
  struct event *event_to_stop = ctx;
  event_del(event_to_stop);
  event_free(event_to_stop);
}

/* Adds or removes Poisson processes to update the query rate. */
static void add_remove_poisson_processes(evutil_socket_t fd, short events, void *ctx)
{
  /* How many (positive or negative) poisson processes should we create? */
  int *nb_poisson_change = ctx;
  if (*nb_poisson_change > 0) {
    debug("Adding %d poisson processes\n", *nb_poisson_change);
    for (int i = 0; i < *nb_poisson_change; i++) {
      add_poisson_sender();
    }
  }
  if (*nb_poisson_change < 0) {
    debug("Removing %d poisson processes\n", -(*nb_poisson_change));
    for (int i = 0; i < -(*nb_poisson_change); i++) {
      poisson_remove(1);
    }
  }
}

/* Called once, and starts a recurrent event that periodically adds or
   removes Poisson processes to implement a rate slope change. */
static void change_query_rate_slope(evutil_socket_t fd, short events, void *ctx)
{
  struct rateslope_command *command = ctx;
  struct event *recurr_ev;
  unsigned long int repeat_interval_us;
  struct timeval repeat_interval = {0, 0};
  struct event *stop_ev;
  struct timeval stop_delay = {0, 0};
  /* Instructed to do nothing, let's do it. */
  if (command->query_rate_slope == 0) {
    info("Resetting query slope to 0 qps/s\n");
    return;
  }
  /* TODO: where should we free nb_poisson_change? */
  int *nb_poisson_change = malloc(sizeof(int));
  /* Schedule recurrent event to add or remove poisson processes */
  /* Compute jointly the interval between updates, and the number of
     poisson processes to add/remove at each update point.  The goal is to
     target an update interval of 100 ms (RATE_SLOPE_UPDATE_INTERVAL_MSEC)
     but the actual value will be slightly different: for instance, to
     reach a slope of +42 qps/s, we add 4 poisson processes every 95.2 ms. */
  *nb_poisson_change = divide_closest(command->query_rate_slope * RATE_SLOPE_UPDATE_INTERVAL_MSEC, 1000);
  if (*nb_poisson_change == 0)
    *nb_poisson_change = command->query_rate_slope > 0 ? 1 : -1;
  repeat_interval_us = 1000 * 1000 * (*nb_poisson_change) / command->query_rate_slope;
  info("Changing query rate slope to %d qps/s (%d Poisson processes every %lu.%.3lu ms)\n",
       command->query_rate_slope,
       *nb_poisson_change,
       repeat_interval_us / 1000,
       repeat_interval_us % 1000);
  recurr_ev = event_new(base, -1, EV_PERSIST, add_remove_poisson_processes, nb_poisson_change);
  timeval_add_us(&repeat_interval, repeat_interval_us);
  event_add(recurr_ev, &repeat_interval);
  /* Schedule removal of the repeating event */
  stop_ev = event_new(base, -1, 0, stop_event, recurr_ev);
  timeval_add_ms(&stop_delay, command->duration_ms);
  event_add(stop_ev, &stop_delay);
  /* TODO: where should we free stop_ev? */
}
