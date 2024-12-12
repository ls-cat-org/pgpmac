/*! \file lslogging.c
 * \brief Logs messages to a file
 * \date 2012
 * \author Keith Brister
 * \copyright All Rights Reserved
 */

#include <stdbool.h>
#include "pgpmac.h"

static pthread_t       lslogging_thread;	//!< our thread
static pthread_mutex_t lslogging_mutex;		//!< mutex to keep the various threads from adding to the queue at the exact same time
static pthread_cond_t  lslogging_cond;		//!< We'll spend most of our time waiting for this condition's signal

//! Fixed maximum length messages to keep some form of sanity
#define LSLOGGING_MSG_LENGTH 2048

/** Our log object: timestamp and message
 */
typedef struct lslogging_queue_struct {
  struct timespec ltime;		//!< time stamp: set when queued
  char lmsg[LSLOGGING_MSG_LENGTH];	//!< our message, truncated if too long
} lslogging_queue_t;

//! Statically-allocated circular buffer queue.
#define LSLOGGING_QUEUE_LENGTH 8192
static lslogging_queue_t lslogging_queue[LSLOGGING_QUEUE_LENGTH];

static unsigned int lslogging_on  = 0;	//!< next location to add to the queue
static unsigned int lslogging_off = 0;	//!< next location to remove from the queue

static regex_t lslogging_ignore_regex;
static const char lslogging_ignorable[] = "^.*I5112=\\(4000\\*8388607/I10\\).*$|^EVENT: Heartbeat$|^EVENT: Check Detector Position$";

//! Initialize the lslogging objects
void lslogging_init() {
  static const char id[] = FILEID "lslogging_init";
  int err;

  pthread_mutex_init( &lslogging_mutex, NULL);
  pthread_cond_init(  &lslogging_cond, NULL);

  openlog("pgpmac", LOG_PID, LOG_USER);

  err = regcomp(&lslogging_ignore_regex, lslogging_ignorable, REG_EXTENDED | REG_NOSUB);
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
  lslogging_on  = 0; //!< next location to add to the queue
  lslogging_off = 0; //!< next location to remove from the queue
}

/** The routine everyone will be talking to.
 *  \param fmt A printf style formating string.
 *  \param ... The arguments specified by fmt
 */
void lslogging_log_message(const char *fmt, ...) {
  static const char syslog_preface[] = "pgpmac: ";
  char syslog_fmt[LSLOGGING_MSG_LENGTH];
  char msg[LSLOGGING_MSG_LENGTH];
  va_list arg_ptr;

  /*
    Write the message to syslog right away (before anything else),
    prepending the name of the program to the syslog message so
    we can easily grep for our messages.

    NOTE: We do not filter out any messages from being written to syslog.
  */
  memcpy(syslog_fmt, syslog_preface, (sizeof(syslog_preface) - 1));
  strncpy(&(syslog_fmt[sizeof(syslog_preface)]), fmt,
	  (sizeof(syslog_fmt) - sizeof(syslog_preface) - 1));
  va_start(arg_ptr, fmt);
  vsyslog(LOG_INFO, syslog_fmt, arg_ptr);
  va_end(arg_ptr);

  va_start(arg_ptr, fmt);
  vsnprintf(msg, sizeof(msg)-1, fmt, arg_ptr);
  va_end(arg_ptr);
  if (regexec(&lslogging_ignore_regex, msg, 0, NULL, 0)) {
    pthread_mutex_lock( &lslogging_mutex);
    {
      // Put the log message on a queue for slower redis and ncurses updates.
      // If the queue is already maxed out, the oldest message gets overwritten.
      lslogging_on = (lslogging_on + 1) % LSLOGGING_QUEUE_LENGTH;
      if (lslogging_on == lslogging_off) {
	// If our circular buffer is full, the oldest message is now the one
	// "in front" of the one we are inserting.
	lslogging_off = (lslogging_on + 1) % LSLOGGING_QUEUE_LENGTH;
      }

      // Fill in the message.
      strncpy(lslogging_queue[lslogging_on].lmsg, msg, (LSLOGGING_MSG_LENGTH - 1));
      lslogging_queue[lslogging_on].lmsg[LSLOGGING_MSG_LENGTH-1] = 0;
      clock_gettime(CLOCK_REALTIME, &(lslogging_queue[lslogging_on].ltime));
    }
    pthread_mutex_unlock(&lslogging_mutex);
    pthread_cond_signal(&lslogging_cond);
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

/**
 * Service the queue, write to the file.
 * @param dummy unused parameter required by pthread_create()
 */
void *lslogging_worker(void *dummy) {
  struct tm coarsetime;
  char tstr[64];
  unsigned int msecs;
  const char* lmsg;
  int errcode;
  char errbuf[1024];

  // The worker thread holds the lock by default, releases it when the queue is
  // empty
  pthread_mutex_lock(&lslogging_mutex);
  while (true) {
    // Yield until there is something in the queue.
    while (lslogging_on == lslogging_off) {
      errcode = pthread_cond_wait(&lslogging_cond, &lslogging_mutex);
      if (errcode != 0) {
	snprintf(errbuf, (sizeof(errbuf)-1),
		 "pgpmac - lslogging_worker, pthread_cond_wait failed w/"
		 " status %d: %s", errcode, strerror(errcode));
	syslog(LOG_ERR, "%s", errbuf);
	pgpmac_printf("\n%s", errbuf);
	exit(-1);
      }
    }

    // Consume just one entry from the queue.
    lslogging_off = (lslogging_off + 1) % LSLOGGING_QUEUE_LENGTH;
    lmsg = lslogging_queue[lslogging_off].lmsg;
    if (regexec(&lslogging_ignore_regex, lmsg, 0, NULL, 0)) {
      localtime_r( &(lslogging_queue[lslogging_off].ltime.tv_sec), &coarsetime);
      strftime( tstr, sizeof(tstr)-1, "%Y-%m-%d %H:%M:%S", &coarsetime);
      tstr[sizeof(tstr)-1] = 0;
      msecs = lslogging_queue[lslogging_off].ltime.tv_nsec / 1000;
      lsredis_log("%s.%.06u  %s\n", tstr, msecs, lmsg);

      //
      // If the newline comes after the string then only a blank line comes out
      // in the ncurses terminal.  Don't know why.
      //
      pgpmac_printf( "\n%s", lmsg);
    }
  }
  pthread_mutex_unlock(&lslogging_mutex);
}


/** Start up the worker thread.
 */
pthread_t *lslogging_run() {
  pthread_create( &lslogging_thread, NULL, &lslogging_worker, NULL);
  lslogging_log_message( "Starting up");
  lsevents_add_listener( ".+", lslogging_event_cb);
  return &lslogging_thread;
}
