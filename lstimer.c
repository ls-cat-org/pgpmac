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

static pthread_t lstimer_thread;		//!< the timer thread
static pthread_mutex_t lstimer_mutex;		//!< protect the timer list
static pthread_cond_t  lstimer_cond;		//!< allows us to be idle when there is nothing to do
static timer_t lstimer_timerid;			//!< our real time timer
static int new_timer    = 0;			//!< indicate that a new timer exists and a call to service_timers is required
static int check_timers = 0;			//!< set by timer interupt
static int got_signal   = 0;

/** Unsets all timers for the given event
 */
void lstimer_unset_timer( char *event) {
  int i;

  pthread_mutex_lock( &lstimer_mutex);

  for( i=0; i<LSTIMER_LIST_LENGTH; i++) {
    if( strcmp( event, lstimer_list[i].event) == 0 && lstimer_list[i].shots != 0) {
      lstimer_list[i].shots = 0;
      if (lstimer_active_timers > 0) {
	lstimer_active_timers--;
      }
      if (lstimer_active_timers == 0) {
	break;
      }
    }
  }

  // Set this flag and trigger the signal in case a timer interupt
  // came in while we were futzing around.
  //
  check_timers = 1;
  pthread_cond_signal( &lstimer_cond);

  pthread_mutex_unlock( &lstimer_mutex);
}


/** Create a timer
 * \param event  Name of the event to send when the timer goes off
 * \param shots  Number of times to run.  0 means never, -1 means forever
 * \param secs   Number of seconds to wait
 * \param nsecs  Number of nano-seconds to run in addition to secs
 */
void lstimer_set_timer( char *event, int shots, unsigned long int secs, unsigned long int nsecs) {
  static const char *id = FILEID "lstimer_set_timer";
  int i;
  struct timespec now;

  // shots == 0 is a no-op
  //
  if (shots == 0) {
    lslogging_log_message("%s: tried to set a timer with 0 shots for event %s", id, event);
    return;
  }


  // Time we were called.  Delay is based on call time, not queued time
  //
  clock_gettime( CLOCK_REALTIME, &now);
  
  pthread_mutex_lock( &lstimer_mutex);

  do {
    // Make sure our event is registered (saves a tiny bit of time later)
    //
    // Return value is the current list of callbacks trigger by our
    // event, Possibly NULL.
    //
    lsevents_preregister_event( event);
    
    //
    // See if we already have an active timer for this event.
    //
    for (i=0; i<LSTIMER_LIST_LENGTH; i++) {
      if (strcmp(lstimer_list[i].event, event) == 0) {
	if (lstimer_list[i].shots != 0) {
	  // We'll increment this further down so if we don't do this
	  // now we'll be incorrectly be adding one later.
	  lstimer_active_timers--;
	  new_timer--;
	}
	break;
      }
    }
    
    //
    // No existing timer for this event exists.  Take over the first
    // inactive time we can find.
    //
    if (i == LSTIMER_LIST_LENGTH) {
      for( i=0; i<LSTIMER_LIST_LENGTH; i++) {
	if( lstimer_list[i].shots == 0)
	  break;
      }
    }
    
    //
    // Well, this is awkward.  There are no more timer slots available.
    // Probably there is a big bad bug somewhere.
    //
    if( i == LSTIMER_LIST_LENGTH) {
      lslogging_log_message("%s: out of timers for event: %s, shots: %d,  secs: %u, nsecs: %u",
			     id, event, shots, secs, nsecs);
      break;
    }

    //
    // Set up our new timer
    //
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
    
    lstimer_active_timers++;
    new_timer++;
  } while (0);

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
  for( i=0; (found_active < lstimer_active_timers) && (i<LSTIMER_LIST_LENGTH); i++) {
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
	  //
	  // And rejigger found_active so we do not stop looking for active timers too soon
	  //
	  found_active--;
	} else {
	  p->next_secs  = p->init_secs + (p->ncalls+1) * p->delay_secs + (p->init_nsecs + (p->ncalls+1)*p->delay_nsecs)/1000000000;
	  p->next_nsecs = (p->init_nsecs + (p->ncalls+1)*p->delay_nsecs) % 1000000000;
	}
      }

      //
      // See when we need to come back here again next
      //
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

  //
  // set up the next interupt
  //
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
  got_signal = 1;
}

/** Our worker.
 *  The main loop runs when a new timer is added.
 *  The service routine deals with maintenance.
 */
static void *lstimer_worker(
		     void *dummy		//!< [in] required by protocol
		     ) {
  static const char *id = FILEID "lstimer_worker";
  struct sigevent  sev;
  struct sigaction sa;
  sigset_t mask;
  struct timespec timeout, now;
  int err;

  // See example at http://www.kernel.org/doc/man-pages/online/pages/man2/timer_create.2.html
  //

  // Set up hander
  //
  sa.sa_flags = SA_SIGINFO;
  sa.sa_sigaction = handler;
  sigemptyset(&sa.sa_mask);
  if (sigaction(SIGRTMIN, &sa, NULL) == -1) {
    lslogging_log_message("%s: sigaction failed", id);
    exit( -1);
  }

  // Create the timer
  //
  sev.sigev_notify = SIGEV_SIGNAL;
  sev.sigev_signo  = SIGRTMIN;
  sev.sigev_value.sival_ptr = &lstimer_timerid;
  timer_create( CLOCK_REALTIME, &sev, &lstimer_timerid);


  //
  // Prepare to block the timer signal while in the service routine
  //
  sigemptyset( &mask);
  sigaddset( &mask, SIGRTMIN);
  
  while( 1) {
    pthread_mutex_lock( &lstimer_mutex);

    clock_gettime( CLOCK_REALTIME, &now);
    timeout.tv_sec  = now.tv_sec + 1;
    timeout.tv_nsec = now.tv_nsec;

    err = 0;
    while( new_timer == 0 && check_timers == 0 && err == 0) {
      err = pthread_cond_timedwait( &lstimer_cond, &lstimer_mutex, &timeout);
    }

    //
    // Should probably only go on timeout if got_signal is non zero
    // but there should be no harm done if it's zero as long as the
    // timeout interval is not too short
    //
    if (err == ETIMEDOUT) {
      got_signal = 0;
    } else {
      if (err != 0) {
	continue;
      }
    }

    // ignore signals so we don't service the signal while we are already in the
    // service routine
    //
    pthread_sigmask( SIG_SETMASK, &mask, NULL);

    //
    // Setting up the timer interval is in the handler
    // so just call it
    //
    service_timers();

    //
    // Reset our flags
    //
    new_timer = 0;
    check_timers = 0;

    pthread_mutex_unlock( &lstimer_mutex);


    // Let the signals rain down
    //
    pthread_sigmask( SIG_UNBLOCK, &mask, NULL);
  }
}


/** Initialize the timer list and pthread stuff.
 */
void lstimer_init() {
  int i;
  pthread_mutexattr_t mutex_initializer;

  pthread_mutexattr_init( &mutex_initializer);
  pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);

  for( i=0; i<LSTIMER_LIST_LENGTH; i++) {
    lstimer_list[i].shots = 0;
  }

  //
  // recursive is needed to handle a race condition where the interupt
  // triggers when we have the mutex but before we can turn off the
  // signal.
  //
  pthread_mutex_init( &lstimer_mutex, &mutex_initializer);
  pthread_cond_init(  &lstimer_cond, NULL);
}

/** Start up our thread.
 */
pthread_t *lstimer_run() {
  pthread_create( &lstimer_thread, NULL, lstimer_worker, NULL);
  return &lstimer_thread;
}
