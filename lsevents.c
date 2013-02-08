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

/** linked list of callbacks for each event
 *   Used to find all the callbacks for a given event
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
 *  \param event the name of the event to listen for
 *  \param cb the routine to call
 */
void lsevents_add_listener( char *event, void (*cb)(char *)) {
  lsevents_listener_t *new;
  int err;
  char *errbuf;
  int nerrbuf;



  new = calloc( 1, sizeof( lsevents_listener_t));
  if( new == NULL) {
    lslogging_log_message( "lsevents_add_listener: out of memory");
    exit( -1);
  }

  err = regcomp( &new->re, event, REG_EXTENDED | REG_NOSUB);
  if( err != 0) {
    nerrbuf = regerror( err, &new->re, NULL, 0);
    errbuf = calloc( nerrbuf, sizeof( char));
    if( errbuf == NULL) {
      lslogging_log_message( "lsevents_add_listener: out of memory (re)");
      exit( -1);
    }
    regerror( err, &new->re, errbuf, nerrbuf);
    lslogging_log_message( "lsevents_add_listener: %s", errbuf);
    free( errbuf);
    free( new);
    return;
  }

  new->raw_regexp = strdup( event);
  new->cb   = cb;

  pthread_mutex_lock( &lsevents_listener_mutex);
  new->next = lsevents_listeners_p;
  lsevents_listeners_p = new;
  pthread_mutex_unlock( &lsevents_listener_mutex);

  // TODO: go through list of event names and perhaps add ourselves to the callback list
  //

  lslogging_log_message( "lsevents_add_listener: added listener for event %s", event);

}

/** Remove a listener previously added with lsevents_add_listener
 *  \param event The name of the event
 *  \param cb The callback routine to remove
 */
void lsevents_remove_listener (char *event, void (*cb)(char *)) {
  
  lsevents_listener_t *last, *current;

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
  }
  pthread_mutex_unlock( &lsevents_listener_mutex);

  //
  // Now remove it
  //
  if( current != NULL) {
    if( current->raw_regexp != NULL)
      free( current->raw_regexp);
    free(current);
  }

  // TODO: go through list of event names and perhaps remove ourselves to the callback list
  //


}

/** Our worker
 *  \param dummy Unused but needed by pthreads to be happy
 */
void *lsevents_worker(
		     void *dummy
		     ) {
  
  lsevents_listener_t *p;
  ENTRY entry_in, *entry_outp;
  char *event;
  int err;
  lsevents_callbacks_t *last_cb, *new_cb, *cbi;
  lsevents_event_names_t *new_event_name, *event_name_list;

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


    // Have we seen this event before?
    //
    entry_in.key  = event;
    entry_in.data = NULL;

    //
    // protect the hash table: add_listener and remove_listener may also mess with it
    //
    pthread_mutex_lock( &lsevents_listener_mutex);
    err = hsearch_r( entry_in, FIND, &entry_outp, &lsevents_event_name_ht);
    if( err == 0) {
      if( errno != ESRCH) {
	lslogging_log_message( "lsevents_worker: hsearch_r returned %d: %s", errno, strerror( errno));
	free( event);
	continue;
      } else {
	//
	// Not Found
	//
	// Make an array of matching events;
	//
	last_cb = NULL;
	
	for( p=lsevents_listeners_p; p != NULL; p = p->next) {
	  if( regexec( &p->re, event, 0, NULL, 0) == 0) {
	    new_cb = calloc( 1, sizeof( lsevents_callbacks_t));
	    new_cb->cb = p->cb;
	    new_cb->next = last_cb;
	    last_cb = new_cb;
	  }
	}
	new_event_name = calloc( 1, sizeof( lsevents_event_names_t));
	new_event_name->event = strdup( event);
	new_event_name->cbl   = last_cb;
	new_event_name->next  = lsevents_event_names;
	lsevents_event_names  = new_event_name;

	entry_in.key  = new_event_name->event;
	entry_in.data = new_event_name;
	hsearch_r( entry_in, ENTER, &entry_outp, &lsevents_event_name_ht);
	if( ++lsevents_n_events  >= lsevents_max_events) {
	  hdestroy_r( &lsevents_event_name_ht);
	  lsevents_max_events *= 2;
	  hcreate_r( lsevents_max_events * 2, &lsevents_event_name_ht);
	  for( new_event_name = lsevents_event_names; new_event_name != NULL; new_event_name = new_event_name->next) {
	    entry_in.key  = new_event_name->event;
	    entry_in.data = new_event_name;
	    hsearch_r( entry_in, ENTER, &entry_outp, &lsevents_event_name_ht);
	  }
	}
	err = hsearch_r( entry_in, FIND, &entry_outp, &lsevents_event_name_ht);
	if( err == 0) {
	  lslogging_log_message( "lsevents_worker: For some reason we can find a hash table entry for event '%s'", event);
	  free( event);
	  pthread_mutex_unlock( &lsevents_listener_mutex);
	  continue;
	}
      }
    }
    pthread_mutex_unlock( &lsevents_listener_mutex);


    // call our callbacks
    //
    event_name_list = entry_in.data;
    for( cbi = event_name_list->cbl; cbi != NULL; cbi = cbi->next) {
      cbi->cb( event);
    }

    free( event);
  }
  return NULL;
}

/** Initialize this module
 */
void lsevents_init() {
  pthread_mutex_init( &lsevents_queue_mutex, NULL);
  pthread_cond_init(  &lsevents_queue_cond, NULL);
  pthread_mutex_init( &lsevents_listener_mutex, NULL);

  hcreate_r( 2*lsevents_max_events, &lsevents_event_name_ht);


       int hcreate_r(size_t nel, struct hsearch_data *htab);

       int hsearch_r(ENTRY item, ACTION action, ENTRY **retval,
                     struct hsearch_data *htab);

       void hdestroy_r(struct hsearch_data *htab);
  
}

/** Start up the thread and get out of the way.
 */
void lsevents_run() {
  pthread_create( &lsevents_thread, NULL, lsevents_worker, NULL);
}
