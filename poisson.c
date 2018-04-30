#include "poisson.h"
#include "utils.h"


/* Array of all poisson processes */
static struct poisson_process* _processes;
static size_t _processes_size;
/* Next process ID available */
static unsigned int _next_process_id;


static struct poisson_process* _get_process(unsigned int process_id)
{
  if (process_id >= _next_process_id) {
    return NULL;
  }
  return &_processes[process_id];
}

static int _increase_processes(size_t new_size)
{
  struct poisson_process* ret;
  if (new_size < 2 * _processes_size) {
    new_size = 2 * _processes_size;
  }
  ret = realloc(_processes, new_size * sizeof(struct poisson_process));
  if (ret == NULL) {
    return -1;
  }
  _processes = ret;
  _processes_size = new_size;
  return 0;
}

static void poisson_event(evutil_socket_t fd, short events, void *ctx)
{
  unsigned int process_id = ctx;
  struct poisson_process *proc = _get_process(process_id);
  static struct timeval interval;
  /* Schedule next query */
  generate_poisson_interarrival(&interval, proc->rate);
  int ret = event_add(proc->event, &interval);
  if (ret != 0) {
    fprintf(stderr, "Failed to schedule next query (Poisson process %u)\n", proc->process_id);
  }
  /* Run user-provided callback function */
  if (proc->callback != NULL) {
    proc->callback(proc->callback_arg);
  }
}


/* Initialize the Poisson framework.  The number of Poisson processes is
   indicative, and should be set to the expected number of processes to
   avoid needless memory reallocations. */
int poisson_init(size_t nb_poisson_processes)
{
  _processes = malloc(nb_poisson_processes * sizeof(struct poisson_process));
  _processes_size = nb_poisson_processes;
  _next_process_id = 0;
  return (_processes != NULL);
}

/* Stops all events and frees all datastructures. */
void poisson_destroy()
{
  while (poisson_remove() != -1);
  free(_processes);
}

/* Returns the process ID of the newly created Poisson process */
unsigned int poisson_new(struct event_base *base)
{
  struct poisson_process *proc;
  unsigned int process_id = _next_process_id;
  if (process_id >= _processes_size) {
    _increase_processes(process_id + 1);
  }
  proc = _get_process(process_id);
  proc->process_id = process_id;
  proc->evbase = base;
  proc->rate = 1.;
  proc->callback = NULL;
  proc->callback_arg = NULL;
  proc->event = event_new(proc->evbase, -1, 0, poisson_event, process_id);
  _next_process_id++;
  return process_id;
}

/* Remove a poisson process, and free its callback event. Returns the
   process ID of the removed process, or -1 if none exists. */
int poisson_remove()
{
  struct poisson_process *proc;
  if (_next_process_id == 0)
    return -1;
  _next_process_id--;
  proc = _get_process(_next_process_id);
  if (proc->event != NULL) {
    event_del(proc->event);
    event_free(proc->event);
  }
  return _next_process_id;
}

/* Sets the callback that will be called at Poisson-spaced time intervals */
int poisson_set_callback(unsigned int process_id, callback_fn callback, void* callback_arg)
{
  struct poisson_process *proc = _get_process(process_id);
  if (proc == NULL) {
    return -1;
  }
  proc->callback = callback;
  proc->callback_arg = callback_arg;
  return 0;
}

int poisson_set_rate(unsigned int process_id, double poisson_rate)
{
  struct poisson_process *proc = _get_process(process_id);
  if (proc == NULL) {
    return -1;
  }
  proc->rate = poisson_rate;
  return 0;
}

/* Starts the process.  If [initial_delay] is NULL, generate an initial
   delay according to the poisson process. */
int poisson_start_process(unsigned int process_id, struct timeval* initial_delay)
{
  struct poisson_process *proc = _get_process(process_id);
  struct timeval poisson_delay;
  if (proc == NULL) {
    return -1;
  }
  if (initial_delay != NULL) {
    return event_add(proc->event, initial_delay);
  } else {
    generate_poisson_interarrival(&poisson_delay, proc->rate);
    return event_add(proc->event, &poisson_delay);
  }
}
