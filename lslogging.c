/*! \file lslogging.c
 * \brief Logs messages to a file
 * \date 2012
 * \author Keith Brister
 * \copyright All Rights Reserved
 */

#include "pgpmac.h"

static pthread_t       lslogging_thread;	//!< our thread
static pthread_mutex_t lslogging_mutex;		//!< mutex to keep the various threads from adding to the queue at the exact same time
static pthread_cond_t  lslogging_cond;		//!< We'll spend most of our time waiting for this condition's signal

//! Fixed maximum length messages to keep some form of sanity
#define LSLOGGING_MSG_LENGTH 2048

/** Our log object: time and message
 */
typedef struct lslogging_queue_struct {
  struct timespec ltime;		//!< time stamp: set when queued
  char lmsg[LSLOGGING_MSG_LENGTH];	//!< our message, truncated if too long
} lslogging_queue_t;

//! Modest length queue
#define LSLOGGING_QUEUE_LENGTH 8192
static lslogging_queue_t lslogging_queue[LSLOGGING_QUEUE_LENGTH];	//!< Our entire queue.  Right here.  Every message we'll ever write.

static unsigned int lslogging_on = 0;	//!< next location to add to the queue
static unsigned int lslogging_off= 0;	//!< next location to remove from the queue

static regex_t lslogging_ignore_regex;
static char *lslogging_ignorable = "^.*I5112=\\(4000\\*8388607/I10\\).*$|^EVENT: Heartbeat$|^EVENT: Check Detector Position$";

//! Initialize the lslogging objects
void lslogging_init() {
  static const char *id = FILEID "lslogging_init";
  int err;

  pthread_mutex_init( &lslogging_mutex, NULL);
  pthread_cond_init(  &lslogging_cond, NULL);

  openlog("pgpmac", LOG_PID, LOG_USER);

  err = regcomp (&lslogging_ignore_regex, lslogging_ignorable, REG_EXTENDED | REG_NOSUB);
  if (err != 0) {
    int nerrmsg;
    char *errmsg;
    
    nerrmsg = regerror(err, &lslogging_ignore_regex, NULL, 0);
    if (nerrmsg > 0) {
      errmsg = calloc(nerrmsg, sizeof(char));
      nerrmsg = regerror(err, &lslogging_ignore_regex, errmsg, nerrmsg);
      syslog(LOG_INFO, "%s initialization error: problem with regexp: %s\n", id, errmsg);
      free(errmsg);
    }
  }
}

/** The routine everyone will be talking to.
 *  \param fmt A printf style formating string.
 *  \param ... The arguments specified by fmt
 */
void lslogging_log_message(char *fmt, ...) {
  static const char *id = FILEID "lslogging_log_message";
  struct timespec theTime;
  char msg[LSLOGGING_MSG_LENGTH];
  unsigned int on;
  va_list arg_ptr;

  (void) id;

  clock_gettime( CLOCK_REALTIME, &theTime);

  va_start( arg_ptr, fmt);
  vsnprintf( msg, sizeof(msg)-1, fmt, arg_ptr);
  va_end( arg_ptr);
  msg[sizeof(msg)-1]=0;

  if (regexec(&lslogging_ignore_regex, msg, 0, NULL, 0)) {
    va_start(arg_ptr, fmt);
    vsyslog(LOG_INFO, fmt, arg_ptr);
    va_end(arg_ptr);

    pthread_mutex_lock( &lslogging_mutex);
    
    on = (lslogging_on++) % LSLOGGING_QUEUE_LENGTH;
    strncpy( lslogging_queue[on].lmsg, msg, LSLOGGING_MSG_LENGTH - 1);
    lslogging_queue[on].lmsg[LSLOGGING_MSG_LENGTH-1] = 0;
    
    memcpy( &(lslogging_queue[on].ltime), &theTime, sizeof(theTime));
    pthread_cond_signal(  &lslogging_cond);
    pthread_mutex_unlock( &lslogging_mutex);
  }
}

/** Log most events
 */
void lslogging_event_cb( char *event) {
  if(
     strcmp(event, "Timer Update KVs") != 0
     && strstr(event, "accepted")==NULL
     && strstr(event, "queued")==NULL
     && strstr(event, "Check Detector Position") == NULL
     && strstr(event, "Heartbeat")==NULL
     && strstr(event, "DETECTOR_STATE_MACHINE")==NULL) {
    lslogging_log_message("EVENT: %s", event);
  }
}

/** Service the queue, write to the file.
 */
void *lslogging_worker(
		      void *dummy	/**< [in] Required by protocol but unused	*/
		      ) {

  struct tm coarsetime;
  char tstr[64];
  unsigned int msecs;
  unsigned int off;

  pthread_mutex_lock( &lslogging_mutex);

  while( 1) {
    while( lslogging_on == lslogging_off) {
      pthread_cond_wait( &lslogging_cond, &lslogging_mutex);
    }
    
    off = (lslogging_off++) % LSLOGGING_QUEUE_LENGTH;

    if (regexec(&lslogging_ignore_regex, lslogging_queue[off].lmsg, 0, NULL, 0)) {
      localtime_r( &(lslogging_queue[off].ltime.tv_sec), &coarsetime);
      strftime( tstr, sizeof(tstr)-1, "%Y-%m-%d %H:%M:%S", &coarsetime);
      tstr[sizeof(tstr)-1] = 0;
      msecs = lslogging_queue[off].ltime.tv_nsec / 1000;
      
      lsredis_log( "%s.%.06u  %s\n", tstr, msecs, lslogging_queue[off].lmsg);
      
      //
      // If the newline comes after the string then only a blank line comes out
      // in the ncurses terminal.  Don't know why.
      //
      pgpmac_printf( "\n%s", lslogging_queue[off].lmsg);
    }
  }
}


/** Start up the worker thread.
 */
pthread_t *lslogging_run() {
  pthread_create( &lslogging_thread, NULL, &lslogging_worker, NULL);
  lslogging_log_message( "Starting up");
  lsevents_add_listener( ".+", lslogging_event_cb);
  return &lslogging_thread;
}
