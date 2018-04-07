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
  static const char *id = FILEID "detector_state_push_queue";

  (void)id;

  pthread_mutex_lock( &detector_state_queue_mutex);

  detector_state_queue_on++;
  
  pthread_cond_signal(  &detector_state_queue_cond);
  pthread_mutex_unlock( &detector_state_queue_mutex);
}

int detector_state_machine_state() {
  static const char *id = FILEID "detector_state_machine_state";
  static char *(the_states[]) = {"Init", "Ready", "Arm", "Armed", "Done", NULL};
  int i;
  char *detector_state_text;
  long long expires;
  time_t now_time;
  long long now;
  int newTimeout;
  char *tmp_string;
  json_t *detector_state_obj;
  json_error_t json_err;
  json_t *detector_state_json;
  json_t *detector_expires_json;
  int set_new_state;
    
  detector_expires_json = NULL;
  detector_state_json   = NULL;
  detector_state_obj    = NULL;
  detector_state_text   = NULL;

  newTimeout    = 10;
  
  do {
    detector_state_text    = lsredis_getstr( detector_state_redis);
    detector_state_obj     = json_loads(detector_state_text, JSON_DECODE_INT_AS_REAL, &json_err);
    if (!detector_state_obj) {
      lslogging_log_message( "%s: Could not decode detector state: %s", id, detector_state_text);
      break;
    }
    
    detector_state_json = json_object_get(detector_state_obj, "state");
    if (!detector_state_json || json_typeof(detector_state_json) != JSON_STRING) {
      lslogging_log_message( "%s: Could not find detector state in json dict: %s", id, detector_state_text);
      break;
    }
    
    detector_expires_json = json_object_get(detector_state_obj,"expires");
    if (!detector_expires_json || json_typeof(detector_expires_json) != JSON_REAL) {
      lslogging_log_message( "%s: Could not find legal detector expires in json dict: %s  got type %d",
			     id, detector_state_text, detector_expires_json ? json_typeof(detector_expires_json) : -1);
      break;
    }
    
    set_new_state = 0;
    
    for (i=0; the_states[i]; i++) {
      if (strcmp(the_states[i], json_string_value(detector_state_json)) == 0) {
	detector_state_int = i;
	break;
      }
    }
    //
    // Did we really find a new state?  If not then we must go to the Init state
    //
    if (the_states[i] == NULL) {
      // went past the end of the list
      lslogging_log_message("%s: Failed to find legal state in %s", id, detector_state_text);
      detector_state_int = 0;
    }
    
    expires = (long long)floor(json_real_value(detector_expires_json));
    
    now_time = time(NULL);	// time_t
    now = now_time;		// convert to long long
    
    //
    // Init state does not expire.  If expires is zero or state_int is
    // zero then we do not consider the current state to have expired.
    // Note that the Init state is the only one that does not expire so
    // the following test appears to be a tad redundant.
    //
    if (expires == 0 || detector_state_int == 0) {
      detector_state_expired = 0;
    } else {
      if (expires < now*1000) {
	detector_state_expired = 1;
      } else {
	detector_state_expired = 0;
      }
    }
    
    if (detector_state_expired) {
      //
      // Reset the detector
      //
      json_integer_set(detector_expires_json, 0);
      json_string_set(detector_state_json, "Init");
      
      set_new_state = 1;
      newTimeout = 60;
    } else {
      //
      // Wait a little beyond the expiration timestamp before we check
      // again.
      //
      if (expires) {
	newTimeout = expires/1000 - now + 1;
	if (newTimeout <= 0) {
	  newTimeout = 10;
	}
      } else {
	//
	// Give it 10 seconds
	//
	newTimeout = 10;
      }
    }
    
    json_object_set(detector_state_obj, "state", detector_state_json);
    json_object_set(detector_state_obj, "expires", detector_expires_json);
    
    if (set_new_state) {
      tmp_string = json_dumps(detector_state_obj, 0);
      lsredis_setstr(detector_state_redis, tmp_string);
      free(tmp_string);
    }
  } while(0);

  if (detector_expires_json) {
    json_decref(detector_expires_json);
  }
  if (detector_state_json) {
    json_decref(detector_state_json);
  }
  if (detector_state_obj) {
    json_decref(detector_state_obj);
  }
  if(detector_state_text) {
    free(detector_state_text);
  }

  return newTimeout;
}


/** Our worker
 *  \param dummy Unused but needed by pthreads to be happy
 */
void *detector_state_worker(void *dummy) {
  static const char *id = FILEID "detector_state_worker";
  int newTimeout = 10;

  (void)id;

  detector_state_redis = lsredis_get_obj("detector.state_machine");

  lsredis_set_onSet( lsredis_get_obj("detector.state_machine"), detector_state_push_queue);
  lsevents_add_listener( "^DETECTOR_STATE_MACHINE$",            detector_state_push_queue);

  pthread_mutex_lock( &detector_state_queue_mutex);

  while( 1) {
    //
    // Wait for a request to come in
    //
    while( detector_state_queue_off == detector_state_queue_on)
      pthread_cond_wait( &detector_state_queue_cond, &detector_state_queue_mutex);

    detector_state_queue_off++;

    newTimeout = detector_state_machine_state();

    lstimer_set_timer( "DETECTOR_STATE_MACHINE", 1, newTimeout, 0);
  }
  return NULL;
}

/** Initialize this module
 */
void detector_state_init() {
  static const char *id = FILEID "detector_state_init";
  pthread_mutexattr_t mutex_initializer;

  (void)id;

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
