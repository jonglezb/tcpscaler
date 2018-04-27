#include <time.h>

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

/* Returns the integer that is closest to a/b */
static inline int divide_closest(int a, int b)
{
  int ret = a / b;
  /* Works for both positive and negative numbers */
  if (2 * (a % b) >= b)
    ret += 1;
  return ret;
}
