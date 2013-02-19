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

//! Full name of the log file.  Probably should be in
//! /var/log/pgpmac.
#define LSLOGGING_FILE_NAME "/tmp/pgpmac.log"
static FILE *lslogging_file;			//!< our log file object

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

//! Initialize the lslogging objects
void lslogging_init() {
  pthread_mutex_init( &lslogging_mutex, NULL);
  pthread_cond_init(  &lslogging_cond, NULL);

  lslogging_file = fopen( LSLOGGING_FILE_NAME, "w");
}

/** The routine everyone will be talking about.
 *  \param fmt A printf style formating string.
 *  \param ... The arguments specified by fmt
 */
void lslogging_log_message( char *fmt, ...) {
  char msg[LSLOGGING_MSG_LENGTH];
  struct timespec theTime;
  va_list arg_ptr;
  unsigned int on;

  clock_gettime( CLOCK_REALTIME, &theTime);

  va_start( arg_ptr, fmt);
  vsnprintf( msg, sizeof(msg)-1, fmt, arg_ptr);
  va_end( arg_ptr);
  msg[sizeof(msg)-1]=0;

  pthread_mutex_lock( &lslogging_mutex);
  
  on = (lslogging_on++) % LSLOGGING_QUEUE_LENGTH;
  strncpy( lslogging_queue[on].lmsg, msg, LSLOGGING_MSG_LENGTH - 1);
  lslogging_queue[on].lmsg[LSLOGGING_MSG_LENGTH-1] = 0;
  
  memcpy( &(lslogging_queue[on].ltime), &theTime, sizeof(theTime));

  pthread_cond_signal(  &lslogging_cond);
  pthread_mutex_unlock( &lslogging_mutex);
  
}

/** Log most events
 */
void lslogging_event_cb( char *event) {
  if( strcmp( event, "Timer Update KVs") != 0 && strstr( event, "accepted")==NULL && strstr( event, "queued")==NULL) {
    lslogging_log_message( "EVENT: %s", event);
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

    localtime_r( &(lslogging_queue[off].ltime.tv_sec), &coarsetime);
    strftime( tstr, sizeof(tstr)-1, "%Y-%m-%d %H:%M:%S", &coarsetime);
    tstr[sizeof(tstr)-1] = 0;
    msecs = lslogging_queue[off].ltime.tv_nsec / 1000;
    fprintf( lslogging_file, "%s.%.06u  %s\n", tstr, msecs, lslogging_queue[off].lmsg);
    fflush( lslogging_file);

    //
    // If the newline comes after the string then only a blank line comes out
    // in the ncurses terminal.  Don't know why.
    //
    pgpmac_printf( "\n%s", lslogging_queue[off].lmsg);
  }
}

/** Start up the worker thread.
 */
void lslogging_run() {
  pthread_create( &lslogging_thread, NULL, &lslogging_worker, NULL);
  lslogging_log_message( "Start up");
  lsevents_add_listener( ".+", lslogging_event_cb);
}
