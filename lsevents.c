#include "pgpmac.h"
/*! \file lsevents.c
 *  \brief event subsystem for inter-pgpmac communication
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */


#define LSEVENTS_QUEUE_LENGTH 512

/** Storage definition for the events.
 *  Just a string for now.  Perhaps one day
 *  we'll succumb to the temptation to add an argument
 *  or two.
 */
typedef struct lsevents_queue_struct {
  char *evp;				//!< name of the event
} lsevents_queue_t;

static lsevents_queue_t lsevents_queue[LSEVENTS_QUEUE_LENGTH];		//!< simple list of events
static unsigned int lsevents_queue_on  = 0;				//!< next queue location to write
static unsigned int lsevents_queue_off = 0;				//!< next queue location to read

//
// Store a list of event n_events names in a hash table
// of maximum length max_events
//
static int lsevents_max_events = 1024;
static int lsevents_n_events   =    0;
static struct hsearch_data lsevents_event_name_ht;

/** Linked list of event listeners.
 */
typedef struct lsevents_listener_struct {
  struct lsevents_listener_struct *next;	//!< Next listener
  char *raw_regexp;				//!< the original string sent to us
  regex_t re;					//!< regular expression representing listened for events
  void (*cb)( char *);				//!< call back function
} lsevents_listener_t;

static lsevents_listener_t *lsevents_listeners_p = NULL;	//!< Pointer to the first item in the link list of listeners

/** lsevents linked list of callbacks for each event
 */
typedef struct lsevents_callbacks_struct {
  struct lsevents_callbacks_struct *next;
  void (*cb)( char *);
} lsevents_callbacks_t;


/** linked list of all the event names
 *  used to regenerate the hash table
 */
typedef struct lsevents_event_names_struct {
  struct lsevents_event_names_struct *next;
  char *event;					// event string
  lsevents_callbacks_t *cbl;			// callback list
} lsevents_event_names_t;
static lsevents_event_names_t *lsevents_event_names = NULL;



static pthread_t       lsevents_thread;			//!< thread to run the event queue
static pthread_mutex_t lsevents_listener_mutex;		//!< mutex to protect the listener linked list
static pthread_mutex_t lsevents_queue_mutex;		//!< mutex to protect the event queue
static pthread_cond_t  lsevents_queue_cond;		//!< condition to pause the queue if needed

/** Call the callback routines for the given event.
 * \param fmt a printf style formating string
 * \param ... list of arguments specified by the format string
 */
void lsevents_send_event( char *fmt, ...) {
  char event[LSEVENTS_EVENT_LENGTH];
  va_list arg_ptr;

  va_start( arg_ptr, fmt);
  vsnprintf( event, sizeof(event)-1, fmt, arg_ptr);
  event[sizeof(event)-1]=0;
  va_end( arg_ptr);

  pthread_mutex_lock( &lsevents_queue_mutex);

  // maybe wait for room on the queue
  while( (lsevents_queue_on + 1) % LSEVENTS_QUEUE_LENGTH == lsevents_queue_off % LSEVENTS_QUEUE_LENGTH)
    pthread_cond_wait( &lsevents_queue_cond, &lsevents_queue_mutex);
  
  lsevents_queue[(lsevents_queue_on++) % LSEVENTS_QUEUE_LENGTH].evp = strdup(event);

  pthread_cond_signal(  &lsevents_queue_cond);
  pthread_mutex_unlock( &lsevents_queue_mutex);
}


/** Add a callback routine to listen for a specific event
 *  \param raw_regexp String value of regular expression to listen to
 *  \param cb the routine to call
 */
void lsevents_add_listener( char *raw_regexp, void (*cb)(char *)) {
  lsevents_listener_t    *new;
  lsevents_event_names_t *enp;
  lsevents_callbacks_t   *cbp;
  int err;
  char *errbuf;
  int nerrbuf;



  new = calloc( 1, sizeof( lsevents_listener_t));
  if( new == NULL) {
    lslogging_log_message( "lsevents_add_listener: out of memory");
    exit( -1);
  }

  err = regcomp( &new->re, raw_regexp, REG_EXTENDED | REG_NOSUB);
  if( err != 0) {
    nerrbuf = regerror( err, &new->re, NULL, 0);
    errbuf = calloc( nerrbuf, sizeof( char));
    if( errbuf == NULL) {
      lslogging_log_message( "lsevents_add_listener: out of memory (re)");
      exit( -1);
    }
    regerror( err, &new->re, errbuf, nerrbuf);
    //    lslogging_log_message( "lsevents_add_listener: %s", errbuf);
    free( errbuf);
    free( new);
    return;
  }

  new->raw_regexp = strdup( raw_regexp);
  new->cb   = cb;

  pthread_mutex_lock( &lsevents_listener_mutex);
  new->next = lsevents_listeners_p;
  lsevents_listeners_p = new;

  for( enp = lsevents_event_names; enp != NULL; enp = enp->next) {
    if( regexec( &new->re, enp->event, 0, NULL, 0) == 0) {
      cbp       = calloc( 1, sizeof( lsevents_callbacks_t));
      cbp->cb   = cb;
      cbp->next = enp->cbl;
      enp->cbl  = cbp;
    }
  }
  

  pthread_mutex_unlock( &lsevents_listener_mutex);

  //  lslogging_log_message( "lsevents_add_listener: added listener for event '%s'", raw_regexp);

}

/** Remove a listener previously added with lsevents_add_listener
 *  \param event The name of the event (possibly a regular expression string)
 *  \param cb The callback routine to remove
 */
void lsevents_remove_listener (char *event, void (*cb)(char *)) {
  
  lsevents_listener_t *last, *current;
  lsevents_event_names_t *enp;
  lsevents_callbacks_t   *cbp, *last_cbp;

  //
  // Find the listener to remove
  // and unlink it from the list
  //
  pthread_mutex_lock( &lsevents_listener_mutex);
  last = NULL;
  for( current = lsevents_listeners_p; current != NULL; current = current->next) {
    if( strcmp( last->raw_regexp, event) == 0 && last->cb == cb) {
      if( last == NULL) {
	lsevents_listeners_p = current->next;
      } else {
	last->next = current->next;
      }
      break;
    }
    last = current;
  }

  if( current == NULL) {
    lslogging_log_message( "lsevents_remove_listener: Could not find this listener for event '%s'", event);
    pthread_mutex_unlock( &lsevents_listener_mutex);
    return;
  }

  //
  // Remove callback from lists of event names
  //
  for( enp = lsevents_event_names; enp != NULL; enp = enp->next) {
    if( regexec( &current->re, enp->event, 0, NULL, 0) == 0) {
      last_cbp = NULL;
      for( cbp = enp->cbl; cbp != NULL; cbp = cbp->next) {
	if( cbp->cb == cb) {
	  if( last_cbp == NULL)
	    enp->cbl = NULL;
	  else
	    last_cbp->next = cbp->next;
	  free( cbp);
	  break;
	}
      }
    }
  }


  pthread_mutex_unlock( &lsevents_listener_mutex);

  //
  // Now remove it
  //
  if( current->raw_regexp != NULL)
    free( current->raw_regexp);
  free(current);

}

/** Add a new event name and find matching callbacks as a returned linked list
 *
 */
lsevents_callbacks_t *lsevents_register_event( char *event) {
  ENTRY entry_in, *entry_outp;
  int err;
  lsevents_callbacks_t *new_cb;
  lsevents_event_names_t *new_event_name, *enp;
  lsevents_listener_t *p;


  //
  // Search for event
  //
  entry_in.key  = event;
  entry_in.data = NULL;

  pthread_mutex_lock( &lsevents_listener_mutex);
  err = hsearch_r( entry_in, FIND, &entry_outp, &lsevents_event_name_ht);
  if( err != 0) {
    //
    // Success, we found the entry
    //
    enp = entry_outp->data;
    pthread_mutex_unlock( &lsevents_listener_mutex);
    return enp->cbl;
  }

  if( errno != ESRCH) {
    //
    // Something awful happened.  At least log it
    //
    lslogging_log_message( "lsevents_register_event: hsearch_r returnd %d: %s", errno, strerror( errno));
    pthread_mutex_unlock( &lsevents_listener_mutex);
    return NULL;
  }

  //  lslogging_log_message( "lsevents_register_event: adding event '%s'", event);
  //
  // Not Found
  //
  // Create new event name item
  new_event_name = calloc( 1, sizeof( lsevents_event_names_t));
  new_event_name->event = strdup( event);
  new_event_name->cbl   = NULL;

  //
  // Find matching callbacks
  //
  for( p = lsevents_listeners_p; p != NULL; p = p->next) {
    if( regexec( &p->re, event, 0, NULL, 0) == 0) {
      new_cb = calloc( 1, sizeof( lsevents_callbacks_t));
      new_cb->cb = p->cb;
      new_cb->next = new_event_name->cbl;
      new_event_name->cbl = new_cb;
    }
  }

  //
  // Add the new event to our linked list
  //
  new_event_name->next  = lsevents_event_names;
  lsevents_event_names  = new_event_name;

  //
  // Also add the new event to our hash table
  //
  entry_in.key  = new_event_name->event;
  entry_in.data = new_event_name;
  err = hsearch_r( entry_in, ENTER, &entry_outp, &lsevents_event_name_ht);
  if( err == 0) {
    //
    // Something bad happend but we can still return a valid callback list.  We just can't use the hash table to find it again later
    //
    lslogging_log_message( "lsevents_register_event: Could not add event name: hsearch_r returned %d: %s", errno, strerror( errno));
    pthread_mutex_unlock( &lsevents_listener_mutex);
    return new_event_name->cbl;
  }

  if( ++lsevents_n_events  >= lsevents_max_events) {
    hdestroy_r( &lsevents_event_name_ht);
    lslogging_log_message( "lsevents_register_event: Increasing event name hash table to %d. lsevents_n_events=%d", 2 * lsevents_max_events, lsevents_n_events);
    lsevents_max_events *= 2;
    hcreate_r( lsevents_max_events * 2, &lsevents_event_name_ht);
    for( enp = lsevents_event_names; enp != NULL; enp = enp->next) {
      entry_in.key  = enp->event;
      entry_in.data = enp;
      hsearch_r( entry_in, ENTER, &entry_outp, &lsevents_event_name_ht);
    }
  }
  //  lslogging_log_message( "lsevents_register_event: added event '%s'", event);
  pthread_mutex_unlock( &lsevents_listener_mutex);
  return new_event_name->cbl;
}  


void lsevents_preregister_event( char *fmt, ...) {
  char  s[128];
  va_list arg_ptr;

  va_start( arg_ptr, fmt);
  vsnprintf( s, sizeof( s) - 1, fmt, arg_ptr);
  s[sizeof(s)-1] = 0;
  va_end( arg_ptr);

  lsevents_register_event( s);
}



/** Our worker
 *  \param dummy Unused but needed by pthreads to be happy
 */
void *lsevents_worker(
		     void *dummy
		     ) {
  
  char *event;
  lsevents_callbacks_t *cbi;

  while( 1) {
    pthread_mutex_lock( &lsevents_queue_mutex);

    //
    // wait for someone to send an event
    //
    while( lsevents_queue_off == lsevents_queue_on)
      pthread_cond_wait( &lsevents_queue_cond, &lsevents_queue_mutex);

    //
    // Get our event name
    //
    event = lsevents_queue[(lsevents_queue_off++) % LSEVENTS_QUEUE_LENGTH].evp;

    //
    // let the send event process know there is room on the queue again
    //
    pthread_cond_signal(  &lsevents_queue_cond);
    pthread_mutex_unlock( &lsevents_queue_mutex);

    // call our callbacks
    //
    pthread_mutex_lock( &lsevents_listener_mutex);
    for( cbi = lsevents_register_event( event); cbi != NULL; cbi = cbi->next) {
      cbi->cb( event);
    }
    pthread_mutex_unlock( &lsevents_listener_mutex);

    free( event);
  }
  return NULL;
}

/** Initialize this module
 */
void lsevents_init() {
  pthread_mutexattr_t mutex_initializer;

  // Use recursive mutexs
  //
  pthread_mutexattr_init( &mutex_initializer);
  pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);

  pthread_mutex_init( &lsevents_queue_mutex,    &mutex_initializer);
  pthread_cond_init(  &lsevents_queue_cond,     NULL);
  pthread_mutex_init( &lsevents_listener_mutex, &mutex_initializer);

  hcreate_r( 2*lsevents_max_events, &lsevents_event_name_ht);
}

/** Start up the thread and get out of the way.
 */
void lsevents_run() {
  pthread_create( &lsevents_thread, NULL, lsevents_worker, NULL);
}
