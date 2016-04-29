/*! \file md2cmds.c
 *  \brief Implements commands to run the md2 diffractometer attached to a PMAC controled by postgresql
 *  \date 2016
 *  \author Keith Brister
 *  \copyright Northwestern University All Rights Reserved
 */
#include "pgpmac.h"

static unsigned int detector_state_queue_on  = 0;	//!< next queue location to write
static unsigned int detector_state_queue_off = 0;	//!< next queue location to read



static pthread_t detector_state_thread;	//!< our thread
pthread_mutex_t  detector_state_queue_mutex;		//!< since redis and timer threads also access this
pthread_cond_t   detector_state_queue_cond;		//!< wait for state change during exposure

pthread_mutex_t detector_state_mutex;			//!< since redis and timer threads also access this
pthread_cond_t  detector_state_cond;			//!< wait for state change during exposure
int detector_state_int     = 0;
int detector_state_expired = 0;

lsredis_obj_t *detector_state_redis;



void detector_state_push_queue(char *dummy_event) {

  pthread_mutex_lock( &detector_state_queue_mutex);
  
  detector_state_queue_on++;
  
  pthread_cond_signal(  &detector_state_queue_cond);
  pthread_mutex_unlock( &detector_state_queue_mutex);
}




/** Really stupid JSON parser that only works for the detector state
 ** machine.  TODO: implement an actual solution such as
 ** http://zserge.com/jsmn.html
 */
void detector_state_machine_state() {
  static char *lastState = NULL;
  char *detector_state_text;
  char *tok;
  long long expires;
  time_t now;
  int newTimeout;
  char new_detector_state_text[256];
  
  detector_state_text    = lsredis_getstr( detector_state_redis);

  newTimeout = 10;
  new_detector_state_text[0] = 0;

  if (strstr(detector_state_text, "Init") != NULL) {
    detector_state_int = 0;
  } else if (strstr(detector_state_text, "Ready") != NULL) {
    detector_state_int = 1;
  } else if (strstr(detector_state_text, "Armed") != NULL) {
    // must test Armed before Arm
    detector_state_int = 3;
  } else if (strstr(detector_state_text, "Arm") != NULL) {
    detector_state_int = 2;
  } else if (strstr(detector_state_text, "Done") != NULL) {
    detector_state_int = 4;
  } else {
    // error condition
    detector_state_int = 0;
  }
  
  expires = 0;
  tok = strstr(detector_state_text, "expires");
  if (tok != NULL) {
    tok = strstr( tok, ":");
    if (tok != NULL) {
      expires = strtoll(tok+1, NULL, 10);
    }
  }
    
  now = time(NULL);
    
  if (expires == 0) {
    detector_state_expired = 0;
  } else {
    if (expires < (long long)now*1000) {
      detector_state_expired = 1;
    } else {
      detector_state_expired = 0;
    }
  }
    
  if (detector_state_expired) {
    if (detector_state_int == 4) {
      //
      // Tell the detector to read out, but if it doesn't in 25 seconds just reset things
      //
      snprintf( new_detector_state_text, sizeof(new_detector_state_text)-1, "{\"state\": \"Done\", \"expires\": %lld}", (long long)time(NULL)*1000 + 20000);
      new_detector_state_text[sizeof(new_detector_state_text)-1] = 0;

      newTimeout = 25;
    } else {
      newTimeout = 60;
      snprintf( new_detector_state_text, sizeof(new_detector_state_text)-1, "{\"state\": \"Init\", \"expires\": 0}");
      new_detector_state_text[sizeof(new_detector_state_text)-1] = 0;
    }
  } else {
    if (expires) {
      newTimeout = expires/1000 - now + 1;
    } else {
      newTimeout = 10;
    }

    if (lastState) {
      free(lastState);
    }

    lastState = detector_state_text;
  }

  lstimer_unset_timer( "DETECTOR_STATE_MACHINE");
  lstimer_set_timer( "DETECTOR_STATE_MACHINE", 1, newTimeout, 0);


  if (new_detector_state_text[0]) {
    lsredis_setstr(detector_state_redis, new_detector_state_text);
  }
}


/** Our worker
 *  \param dummy Unused but needed by pthreads to be happy
 */
void *detector_state_worker(
		     void *dummy
		     ) {

  detector_state_redis = lsredis_get_obj("detector.state_machine");

  lsredis_set_onSet( lsredis_get_obj("detector.state_machine"), detector_state_push_queue);
  lsevents_add_listener( "^DETECTOR_STATE_MACHINE$",            detector_state_push_queue);

  while( 1) {
    //
    // Wait for a request to come in
    //
    pthread_mutex_lock( &detector_state_queue_mutex);
    while( detector_state_queue_off == detector_state_queue_on)
      pthread_cond_wait( &detector_state_queue_cond, &detector_state_queue_mutex);

    detector_state_queue_off++;

    pthread_cond_signal(  &detector_state_queue_cond);
    pthread_mutex_unlock( &detector_state_queue_mutex);

    pthread_mutex_lock( &detector_state_mutex);
    detector_state_machine_state();

    pthread_cond_signal( &detector_state_cond);
    pthread_mutex_unlock( &detector_state_mutex);

  }
  return NULL;
}

/** Initialize this module
 */
void detector_state_init() {
  pthread_mutexattr_t mutex_initializer;

  // Use recursive mutexs
  //
  pthread_mutexattr_init( &mutex_initializer);
  pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);

  pthread_mutex_init( &detector_state_queue_mutex,    &mutex_initializer);
  pthread_cond_init(  &detector_state_queue_cond,     NULL);

  pthread_mutex_init( &detector_state_mutex,    &mutex_initializer);
  pthread_cond_init(  &detector_state_cond,     NULL);

  detector_state_queue_on = 1;
}

/** Start up the thread and get out of the way.
 */
pthread_t *detector_state_run() {
  pthread_create( &detector_state_thread, NULL, detector_state_worker, NULL);
  return &detector_state_thread;
}
