#include <time.h>
#include <math.h>
#include <stdlib.h>

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

/* Returns the integer that is closest to a/b */
static inline int divide_closest(int a, int b)
{
  int ret = a / b;
  /* Works for both positive and negative numbers */
  if (2 * (a % b) >= b)
    ret += 1;
  return ret;
}

void subtract_timespec(struct timespec *result, const struct timespec *a, const struct timespec *b);

void timeval_add_ms(struct timeval *a, unsigned int ms);

void timeval_add_us(struct timeval *a, unsigned long int us);

/* Given a [rate], generate an interarrival sample according to a Poisson
   process and store it in [tv]. */
void generate_poisson_interarrival(struct timeval* tv, double rate);
