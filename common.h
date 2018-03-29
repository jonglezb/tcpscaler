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
/* Maximum number of queries "in flight" on a given UDP or TCP connection.
   Computed from MAX_RTT, rate, and nb_conn. */
static uint16_t max_queries_in_flight;
/* How many Poisson processes we are running in parallel, and at which rate. */
static uint32_t nb_poisson_processes = 0;
static double poisson_rate = 1000. / (double) POISSON_PROCESS_PERIOD_MSEC;
/* How many UDP or TCP connections we maintain. */
static uint32_t nb_conn = 0;


/* Given a [rate], generate an interarrival sample according to a Poisson
   process and store it in [tv]. */
void generate_poisson_interarrival(struct timeval* tv, double rate)
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
