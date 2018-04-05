/*! \file lslogging.c
 * \brief Logs messages to a file
 * \date 2012
 * \author Keith Brister
 * \copyright All Rights Reserved
 */

#include "pgpmac.h"


//! Initialize the lslogging objects
void lslogging_init() {
  openlog("pgpmac", LOG_PID, LOG_USER);
}

/** The routine everyone will be talking to.
 *  \param fmt A printf style formating string.
 *  \param ... The arguments specified by fmt
 */
void lslogging_log_message(char *fmt, ...) {
  static const char *id = FILEID "lslogging_log_message";
  va_list arg_ptr;

  (void) id;

  va_start(arg_ptr, fmt);
  vsyslog(LOG_INFO, fmt, arg_ptr);
  va_end(arg_ptr);
}

/** Log most events
 */
void lslogging_event_cb( char *event) {
  if( strcmp( event, "Timer Update KVs") != 0 && strstr( event, "accepted")==NULL && strstr( event, "queued")==NULL && strstr( event, "Heartbeat")==NULL) {
    lslogging_log_message( "EVENT: %s", event);
  }
}



/** Start up the worker thread.
 *
 * No need to run on a separate thread in this syslog world.
 */
pthread_t *lslogging_run() {
  lslogging_log_message( "Start up");
  lsevents_add_listener( ".+", lslogging_event_cb);
  return NULL;
}
