#include <time.h>
#include <event2/event.h>
#include <event2/bufferevent.h>

typedef void (*callback_fn)(void *);

struct poisson_process {
  /* ID of the Poisson process, mostly for logging purpose. */
  uint32_t process_id;
  /* User-defined callback and argument */
  callback_fn callback;
  void* callback_arg;
  /* Used to schedule the next event for this Poisson process. */
  struct event* event;
  /* Rate of the Poisson process, in events/second */
  double rate;
  /* libevent base */
  struct event_base* evbase;
};


/* Initialize the Poisson framework.  The number of Poisson processes is
   indicative, and should be set to the expected number of processes to
   avoid needless memory reallocations. */
int poisson_init(size_t nb_poisson_processes);

/* Stop all events, and optionally free all callback arguments. */
void poisson_destroy(char free_callback_args);

/* Returns a newly created Poisson process, or NULL in case of failure. */
struct poisson_process* poisson_new(struct event_base *base);

/* Remove a poisson process, and optionally free the callback argument.
   Returns the process ID of the removed process, or -1 if none exists. */
int poisson_remove(char free_callback_arg);

/* Sets the callback that will be called at Poisson-spaced time intervals */
int poisson_set_callback(struct poisson_process* process, callback_fn callback, void* callback_arg);

int poisson_set_rate(struct poisson_process* process, double poisson_rate);

/* Starts the process.  If [initial_delay] is NULL, generate an initial
   delay according to the poisson process. */
int poisson_start_process(struct poisson_process* process, struct timeval* initial_delay);

unsigned int poisson_nb_processes();
