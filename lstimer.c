#include "pgpmac.h"
/*! \file lstimer.c
 *  \brief Support for delayed and periodic events
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */


//! We'll allow this many timers. This should be way more than enough.
#define LSTIMER_LIST_LENGTH 1024

/** times within this amount in the future are considered "now"
 * and the events should be called
 */
#define LSTIMER_RESOLUTION_NSECS 100000

static int lstimer_active_timers = 0;	//!< count of the number timers we are tracking

/** Everything we need to know about a timer.
 */
typedef struct lstimer_list_struct {
  int shots;				//!< run this many times: -1 means reload forever, 0 means we are done with this timer and it may be reused
  unsigned long int ncalls;		//!< track how many times we triggered a callback (like an unsigned long int is really needed)
  char event[LSEVENTS_EVENT_LENGTH];	//!< the event to send
  long int next_secs;		//!< epoch (seconds) of next alarm
  long int next_nsecs;		//!< nano seconds of next alarm
  long int delay_secs;		//!< number of seconds for a periodic delay
  long int delay_nsecs;	//!< nano seconds of delay
  long int last_secs;		//!< the last time this timer was triggered
  long int last_nsecs;		//!< the last time this timer was triggered
  long int init_secs;		//!< our initialization time
  long int init_nsecs;		//!< our initialization time
} lstimer_list_t;

static lstimer_list_t lstimer_list[LSTIMER_LIST_LENGTH];	//!< Our timer list

static pthread_t lstimer_thread;	//!< the timer thread
static pthread_mutex_t lstimer_mutex;	//!< protect the timer list
static pthread_cond_t  lstimer_cond;	//!< allows us to be idle when there is nothing to do
static timer_t lstimer_timerid;		//!< our real time timer
static int new_timer = 0;		//!< indicate that a new timer exists and a call to service_timers is required

/** Unsets all timers for the given event
 */
void lstimer_unset_timer( char *event) {
  int i;

  for( i=0; i<LSTIMER_LIST_LENGTH; i++) {
    if( strcmp( event, lstimer_list[i].event) == 0) {
      lstimer_list[i].shots = 0;
    }
  }
}


/** Create a timer
 * \param event  Name of the event to send when the timer goes off
 * \param shots  Number of times to run.  0 means never, -1 means forever
 * \param secs   Number of seconds to wait
 * \param nsecs  Number of nano-seconds to run in addition to secs
 */
void lstimer_set_timer( char *event, int shots, unsigned long int secs, unsigned long int nsecs) {
  int i;
  struct timespec now;

  // Time we were called.  Delay is based on call time, not queued time
  //
  clock_gettime( CLOCK_REALTIME, &now);
  

  // Make sure our event is registered (saves a tiny bit of time later)
  //
  lsevents_preregister_event( event);

  pthread_mutex_lock( &lstimer_mutex);

  for( i=0; i<LSTIMER_LIST_LENGTH; i++) {
    if( lstimer_list[i].shots == 0)
      break;
  }

  if( i == LSTIMER_LIST_LENGTH) {
    pthread_mutex_unlock( &lstimer_mutex);
    
    lslogging_log_message( "lstimer_set_timer: out of timers for event: %s, shots: %d,  secs: %u, nsecs: %u",
			  event, shots, secs, nsecs);
    return;
  }

  strncpy( lstimer_list[i].event, event, LSEVENTS_EVENT_LENGTH - 1);
  lstimer_list[i].event[LSEVENTS_EVENT_LENGTH - 1] = 0;
  lstimer_list[i].shots        = shots;
  lstimer_list[i].delay_secs   = secs;
  lstimer_list[i].delay_nsecs  = nsecs;

  lstimer_list[i].next_secs    = secs + now.tv_sec + (now.tv_nsec + nsecs) / 1000000000;
  lstimer_list[i].next_nsecs   = (now.tv_nsec + nsecs) % 1000000000;
  lstimer_list[i].last_secs    = 0;
  lstimer_list[i].last_nsecs   = 0;
  
  lstimer_list[i].ncalls       = 0;
  lstimer_list[i].init_secs    = now.tv_sec;
  lstimer_list[i].init_nsecs   = now.tv_nsec;

  if( shots != 0) {
    lstimer_active_timers++;
    new_timer++;
  }

  pthread_cond_signal(  &lstimer_cond);
  pthread_mutex_unlock( &lstimer_mutex);
}


/** Send events that are past due, due, or just about to be due.
 */
static void service_timers() {
  int
    i,
    found_active;

  lstimer_list_t *p;
  struct timespec now, then, soonest;
  struct itimerspec its;

  //
  // Did I remind you not to let this thread own the lstimer mutex outside of this
  // service routine when SIGRTMIN is active?
  //

  // Call with lstimer_mutex locked

  clock_gettime( CLOCK_REALTIME, &now);
  //
  // Project a tad into the future
  then.tv_sec  = now.tv_sec + (now.tv_nsec + LSTIMER_RESOLUTION_NSECS) / 1000000000;
  then.tv_nsec = (now.tv_nsec + LSTIMER_RESOLUTION_NSECS) % 1000000000;

  found_active = 0;
  for( i=0; i<lstimer_active_timers; i++) {
    p = &(lstimer_list[i]);
    if( p->shots != 0) {
      found_active++;
      if(  p->next_secs < then.tv_sec || (p->next_secs == then.tv_sec && p->next_nsecs <= then.tv_nsec)) {
	lsevents_send_event( p->event);
	//
	// After sending the event, compute the next time we need to do this
	//
	p->last_secs  = now.tv_sec;
	p->last_nsecs = now.tv_nsec;
	p->ncalls++;
	//
	// Decrement non-infinite loops
	if( p->shots != -1)
	  p->shots--;
	if( p->shots == 0) {
	  //
	  // Take this timer out of the mix
	  lstimer_active_timers--;
	} else {
	  p->next_secs  = p->init_secs + (p->ncalls+1) * p->delay_secs + (p->init_nsecs + (p->ncalls+1)*p->delay_nsecs)/1000000000;
	  p->next_nsecs = (p->init_nsecs + (p->ncalls+1)*p->delay_nsecs) % 1000000000;
	}
      }

      if( found_active == 1) {
	soonest.tv_sec  = p->next_secs;
	soonest.tv_nsec = p->next_nsecs;
      } else {
	if( soonest.tv_sec > p->next_secs || (soonest.tv_sec == p->next_secs && soonest.tv_nsec > p->next_nsecs)) {
	  soonest.tv_sec  = p->next_secs;
	  soonest.tv_nsec = p->next_nsecs;
	}
      }
    }
  }

  if( soonest.tv_sec != 0) {
    its.it_value.tv_sec     = soonest.tv_sec;
    its.it_value.tv_nsec    = soonest.tv_nsec;
    its.it_interval.tv_sec  = 0;
    its.it_interval.tv_nsec = 0;
    timer_settime( lstimer_timerid, TIMER_ABSTIME, &its, NULL);
  }
}

/** Service the signal
 */
static void handler( int sig, siginfo_t *si, void *dummy) {
  pthread_mutex_lock( &lstimer_mutex);
  service_timers();
  pthread_mutex_unlock( &lstimer_mutex);
}

/** Our worker.
 *  The main loop runs when a new timer is added.
 *  The service routine deals with maintenance.
 */
static void *lstimer_worker(
		     void *dummy		//!< [in] required by protocol
		     ) {
  struct sigevent  sev;
  struct sigaction sa;
  sigset_t mask;

  // See example at http://www.kernel.org/doc/man-pages/online/pages/man2/timer_create.2.html
  //

  // Set up hander
  //
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
    lslogging_log_message( "lstimer_worker: sigaction failed");
    exit( -1);
  }

  // Create the timer
  //
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo  = SIGRTMIN;
  sev.sigev_value.sival_ptr = &lstimer_timerid;
  timer_create( CLOCK_REALTIME, &sev, &lstimer_timerid);


  // Block timer signal for now since we really 
  // want to be sure we do not own a lock on the timer mutex
  // while servicing the signal
  //
  sigemptyset( &mask);
  sigaddset( &mask, SIGRTMIN);
  
  while( 1) {
    pthread_mutex_lock( &lstimer_mutex);

    while( new_timer == 0)
      pthread_cond_wait( &lstimer_cond, &lstimer_mutex);

    // ignore signals so we don't service the signal while we are already in the
    // service routine
    //
    sigprocmask( SIG_SETMASK, &mask, NULL);
    

    //
    // Setting up the timer interval is in the handler
    // so just call it
    //
    service_timers();

    //
    // Reset our flag
    //
    new_timer = 0;

    pthread_mutex_unlock( &lstimer_mutex);


    // Let the signals rain down
    //
    sigprocmask( SIG_UNBLOCK, &mask, NULL);
  }
}


/** Initialize the timer list and pthread stuff.
 */
void lstimer_init() {
  int i;

  for( i=0; i<LSTIMER_LIST_LENGTH; i++) {
    lstimer_list[i].shots = 0;
  }


  pthread_mutex_init( &lstimer_mutex, NULL);
  pthread_cond_init(  &lstimer_cond, NULL);
}

/** Start up our thread.
 */
void lstimer_run() {
  pthread_create( &lstimer_thread, NULL, lstimer_worker, NULL);
}
