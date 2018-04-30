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

/* Stops all events and frees all dat astructures. */
void poisson_destroy();

/* Returns the process ID of the newly created Poisson process */
unsigned int poisson_new(struct event_base *base);

/* Remove a poisson process, and free its callback event. Returns the
   process ID of the removed process, or -1 if none exists. */
int poisson_remove();

/* Sets the callback that will be called at Poisson-spaced time intervals */
int poisson_set_callback(unsigned int process_id, callback_fn callback, void* callback_arg);

int poisson_set_rate(unsigned int process_id, double poisson_rate);

/* Starts the process.  If [initial_delay] is NULL, generate an initial
   delay according to the poisson process. */
int poisson_start_process(unsigned int process_id, struct timeval* initial_delay);

unsigned int poisson_nb_processes();
