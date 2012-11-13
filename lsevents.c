#include "pgpmac.h"
/*! \file lsevents.c
 *  \brief event subsystem for inter-pgpmac communication
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */


#define LSEVENTS_QUEUE_LENGTH 256

typedef struct lsevents_queue_struct {
  char event[LSEVENTS_EVENT_LENGTH];	//!< name of the event
} lsevents_queue_t;

static lsevents_queue_t lsevents_queue[LSEVENTS_QUEUE_LENGTH];
static unsigned int lsevents_queue_on  = 0;
static unsigned int lsevents_queue_off = 0;

typedef struct lsevents_listener_struct {
  struct lsevents_listener_struct *next;	//!< Next listener
  char event[LSEVENTS_EVENT_LENGTH];		//!< name of the event we are listening for
  void (*cb)( char *);				//!< call back function
} lsevents_listener_t;

static lsevents_listener_t *lsevents_listeners_p = NULL;

static pthread_t       lsevents_thread;			//!< thread to run the event queue
static pthread_mutex_t lsevents_listener_mutex;		//!< mutex to protect the listener linked list
static pthread_mutex_t lsevents_queue_mutex;		//!< mutex to protect the event queue
static pthread_cond_t  lsevents_queue_cond;		//!< condition to pause the queue if needed

void lsevents_send_event( char *fmt, ...) {
  char event[LSEVENTS_EVENT_LENGTH];
  char *sp;
  va_list arg_ptr;

  va_start( arg_ptr, fmt);
  vsnprintf( event, sizeof(event)-1, fmt, arg_ptr);
  event[sizeof(event)-1]=0;
  va_end( arg_ptr);

  lslogging_log_message( "lsevents_send_event: %s", event);

  pthread_mutex_lock( &lsevents_queue_mutex);

  // maybe wait for room on the queue
  while( lsevents_queue_on + 1 == lsevents_queue_off)
    pthread_cond_wait( &lsevents_queue_cond, &lsevents_queue_mutex);
  
  sp = lsevents_queue[(lsevents_queue_on++) % LSEVENTS_QUEUE_LENGTH].event;
  strncpy( sp, event, LSEVENTS_EVENT_LENGTH);
  sp[LSEVENTS_EVENT_LENGTH - 1] = 0;

  pthread_cond_signal(  &lsevents_queue_cond);
  pthread_mutex_unlock( &lsevents_queue_mutex);

}


void lsevents_add_listener( char *event, void (*cb)(char *)) {
  lsevents_listener_t *new;

  new = calloc( 1, sizeof( lsevents_listener_t));
  if( new == NULL) {
    lslogging_log_message( "lsevents_add_listener: out of memory");
    exit( -1);
  }

  strncpy( new->event, event, LSEVENTS_EVENT_LENGTH);
  new->event[LSEVENTS_EVENT_LENGTH-1] = 0;
  new->cb   = cb;
  new->next = lsevents_listeners_p;

  pthread_mutex_lock( &lsevents_listener_mutex);
  lsevents_listeners_p = new;
  pthread_mutex_unlock( &lsevents_listener_mutex);

  lslogging_log_message( "lsevents_add_listener: added listener for event %s", event);

}

void lsevents_remove_listener (char *event, void (*cb)(char *)) {
  
  lsevents_listener_t *last, *current;

  //
  // Find the listener to remove
  // and unlink it from the list
  //
  pthread_mutex_lock( &lsevents_listener_mutex);
  last = NULL;
  for( current = lsevents_listeners_p; current != NULL; current = current->next) {
    if( strcmp( last->event, event) == 0 && last->cb == cb) {
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
  // TODO: use saner memory management where we allocate many listeners at a time
  // as an array and then just flag the ones that are used
  //
  if( current != NULL) {
    if( current->event != NULL)
      free( current->event);
    free(current);
  }
}

void *lsevents_worker(
		     void *dummy
		     ) {
  
  char event[LSEVENTS_EVENT_LENGTH];
  lsevents_queue_t *ep;
  lsevents_listener_t *p;

  while( 1) {
    pthread_mutex_lock( &lsevents_queue_mutex);

    //
    // wait for someone to send an event
    //
    while( lsevents_queue_off == lsevents_queue_on)
      pthread_cond_wait( &lsevents_queue_cond, &lsevents_queue_mutex);

    //
    // copy event string since the value in the queue may change when
    // we unlock the mutex
    //
    ep = &(lsevents_queue[(lsevents_queue_off++) % LSEVENTS_QUEUE_LENGTH]);
    strncpy( event, ep->event, LSEVENTS_EVENT_LENGTH);
    event[LSEVENTS_EVENT_LENGTH-1] = 0;

    //
    // let the send event process know there is room on the queue again
    //
    pthread_cond_signal(  &lsevents_queue_cond);
    pthread_mutex_unlock( &lsevents_queue_mutex);

    //
    // Find the callbacks and, well, call them back
    //
    pthread_mutex_lock( &lsevents_listener_mutex);
    for( p = lsevents_listeners_p; p != NULL; p = p->next) {
      if( strcmp( event, p->event) == 0) {
	p->cb( p->event);
      }
    }

    pthread_mutex_unlock( &lsevents_listener_mutex);
  }
  return NULL;
}

void lsevents_init() {
  pthread_mutex_init( &lsevents_queue_mutex, NULL);
  pthread_cond_init(  &lsevents_queue_cond, NULL);
  pthread_mutex_init( &lsevents_listener_mutex, NULL);
}

void lsevents_run() {
  pthread_create( &lsevents_thread, NULL, lsevents_worker, NULL);
}
