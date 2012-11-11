/*! \file lslogging.c
 * \brief Logs messages to a file
 * \date 2012
 * \author Keith Brister
 * \copyright All Rights Reserved
 */

#include "pgpmac.h"

static pthread_t        lslogging_thread;
static pthread_mutex_t lslogging_mutex;
static pthread_cond_t  lslogging_cond;


#define LSLOGGING_FILE_NAME "/tmp/pgpmac.log"
static FILE *lslogging_file;

#define LSLOGGING_MSG_LENGTH 256
typedef struct lslogging_queue_struct {
  struct timespec ltime;			//!< time stamp: set when queued
  char lmsg[LSLOGGING_MSG_LENGTH];	//!< our message, truncated if too long
} lslogging_queue_t;

#define LSLOGGING_QUEUE_LENGTH 256
static lslogging_queue_t lslogging_queue[LSLOGGING_QUEUE_LENGTH];

static unsigned int lslogging_on = 0;	//!< next location to add to the queue
static unsigned int lslogging_off= 0;	//!< next location to remove from the queue

void lslogging_init() {
  pthread_mutex_init( &lslogging_mutex, NULL);
  pthread_cond_init(  &lslogging_cond, NULL);

  lslogging_file = fopen( LSLOGGING_FILE_NAME, "w");
}

void lslogging_log_message( char *fmt, ...) {
  char msg[LSLOGGING_MSG_LENGTH];
  struct timespec theTime;
  va_list arg_ptr;

  clock_gettime( CLOCK_REALTIME, &theTime);

  va_start( arg_ptr, fmt);
  vsnprintf( msg, sizeof(msg)-1, fmt, arg_ptr);
  va_end( arg_ptr);
  msg[sizeof(msg)-1]=0;

  pthread_mutex_lock( &lslogging_mutex);
  
  strncpy( lslogging_queue[lslogging_on].lmsg, msg, LSLOGGING_MSG_LENGTH - 1);
  lslogging_queue[lslogging_on].lmsg[LSLOGGING_MSG_LENGTH-1] = 0;
  
  memcpy( &(lslogging_queue[lslogging_on].ltime), &theTime, sizeof(theTime));

  lslogging_on++;
  lslogging_on %= LSLOGGING_QUEUE_LENGTH;

  pthread_cond_signal(  &lslogging_cond);
  pthread_mutex_unlock( &lslogging_mutex);
  
}

void *lslogging_worker(
		      void *dummy	/**< [in] Required by protocol but unused	*/
		      ) {


  struct tm coarsetime;
  char tstr[64];
  unsigned int msecs;

  pthread_mutex_lock( &lslogging_mutex);

  while( 1) {
    while( lslogging_on == lslogging_off) {
      pthread_cond_wait( &lslogging_cond, &lslogging_mutex);
    }
    
    localtime_r( &(lslogging_queue[lslogging_off].ltime.tv_sec), &coarsetime);
    strftime( tstr, sizeof(tstr)-1, "%Y-%m-%d %H:%M:%S", &coarsetime);
    tstr[sizeof(tstr)-1] = 0;
    msecs = lslogging_queue[lslogging_off].ltime.tv_nsec / 1000000;
    fprintf( lslogging_file, "%s.%.03u  %s\n", tstr, msecs, lslogging_queue[lslogging_off].lmsg);
    fflush( lslogging_file);

    lslogging_off++;
    lslogging_off %= LSLOGGING_QUEUE_LENGTH;
  }
}

void lslogging_run() {
  pthread_create( &lslogging_thread, NULL, &lslogging_worker, NULL);
  lslogging_log_message( "Start up");
}
