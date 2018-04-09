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

//
// Redis onSet function.
//
// Called whenever detector.state_machine is set.
//
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
  json_t *detector_int_expires_json;
    
  detector_expires_json = NULL;
  detector_int_expires_json = NULL;
  detector_state_json   = NULL;
  detector_state_obj    = NULL;
  detector_state_text   = NULL;

  newTimeout    = 10;
  
  do {
    //
    // Get our redis varialbe and parse the JSON it is supposed to contain
    //
    detector_state_text    = lsredis_getstr( detector_state_redis);
    detector_state_obj     = json_loads(detector_state_text, JSON_DECODE_INT_AS_REAL, &json_err);
    if (!detector_state_obj) {
      lslogging_log_message( "%s: Could not decode detector state: %s", id, detector_state_text);
      break;
    }
    
    //
    // Parse the state string and convert it to an integer using "the_states"
    //
    detector_state_json = json_object_get(detector_state_obj, "state");
    if (!detector_state_json || json_typeof(detector_state_json) != JSON_STRING) {
      lslogging_log_message( "%s: Could not find detector state in json dict: %s", id, detector_state_text);
      break;
    }
    for (i=0; the_states[i]; i++) {
      if (strcmp(the_states[i], json_string_value(detector_state_json)) == 0) {
	detector_state_int = i;
	break;
      }
    }
    if (the_states[i] == NULL) {
      // went past the end of the list.  Just reset the detector.
      // 
      lslogging_log_message("%s: Failed to find legal state in %s", id, detector_state_text);
      detector_state_int = 0;
    }
    
    // Parse the "expires": the epoch in milliseconds at which the current state expires
    //
    detector_expires_json = json_object_get(detector_state_obj,"expires");
    if (!detector_expires_json || json_typeof(detector_expires_json) != JSON_REAL) {
      lslogging_log_message( "%s: Could not find legal detector expires in json dict: %s  got type %d",
			     id, detector_state_text, detector_expires_json ? json_typeof(detector_expires_json) : -1);
      break;
    }
    expires = (long long)floor(json_real_value(detector_expires_json));
    
    //
    // Init state does not expire.  If 'expires' is zero and
    // 'detector_state_int' is zero then we do not consider the
    // current state to have expired.
    //
    if (expires == 0 && detector_state_int == 0) {
      // There is nothing we need to do here as we are already in the
      // init state with expires=0.
      break;
    }      

    now_time = time(NULL);	// time_t
    now = now_time;		// convert to long long
    
    if (expires < now*1000) {
      //
      // Reset the detector and be done
      //
      detector_int_expires_json = json_integer(0);
      json_object_set(detector_state_obj, "expires", detector_int_expires_json);

      json_string_set(detector_state_json, "Init");
      json_object_set(detector_state_obj, "state",   detector_state_json);

      tmp_string = json_dumps(detector_state_obj, 0);
      lsredis_setstr(detector_state_redis, tmp_string);
      free(tmp_string);

      newTimeout = 60;
      break;
    }

    //
    // Here the current state has not expired so we'll look again
    // later.
    //
    newTimeout = expires/1000 - now + 1;

    if (newTimeout <= 0) {
      newTimeout = 10;
    }
  } while(0);

  if (detector_expires_json) {
    json_decref(detector_expires_json);
  }
  if (detector_int_expires_json) {
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

  lsredis_set_onSet( detector_state_redis, detector_state_push_queue);
  lsevents_add_listener( "^DETECTOR_STATE_MACHINE$", detector_state_push_queue);


  while( 1) {
    //
    // Wait for a request to come in
    //
    pthread_mutex_lock(&detector_state_queue_mutex);
    while( detector_state_queue_off == detector_state_queue_on)
      pthread_cond_wait(&detector_state_queue_cond, &detector_state_queue_mutex);

    detector_state_queue_off++;

    pthread_mutex_unlock(&detector_state_queue_mutex);

    pthread_mutex_lock(&detector_state_mutex);
    newTimeout = detector_state_machine_state();

    pthread_cond_signal(&detector_state_cond);
    pthread_mutex_unlock(&detector_state_mutex);

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
