#include <time.h>
#include <math.h>
#include <stdlib.h>

#include "utils.h"


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

void timeval_add_ms(struct timeval *a, unsigned int ms)
{
  a->tv_usec += ms * 1000;
  while (a->tv_usec >= 1000000L) {
    a->tv_sec += 1;
    a->tv_usec -= 1000000L;
  }
}

void timeval_add_us(struct timeval *a, unsigned long int us)
{
  a->tv_usec += us;
  while (a->tv_usec >= 1000000L) {
    a->tv_sec += 1;
    a->tv_usec -= 1000000L;
  }
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
