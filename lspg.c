//
// lspg.c
//
// Postgresql support for the LS-CAT pgpmac project
// (C) Copyright 2012 by Keith Brister, Northwestern University
// All Rights Reserved
//

#include "pgpmac.h"

/*
  Database state machine

State		Description

 -4		Initiate connection
 -3		Poll until connection initialization is complete
 -2		Initiate reset
 -1		Poll until connection reset is complete
  1		Idle (wait for a notify from the server)
  2		Send a query to the server
  3		Continue flushing a command to the server
  4		Waiting for a reply
  5		Continue waiting for a reply

*/

#define LS_PG_STATE_INIT	-4
#define LS_PG_STATE_INIT_POLL	-3
#define LS_PG_STATE_RESET	-2
#define LS_PG_STATE_RESET_POLL	-1
#define LS_PG_STATE_IDLE	1
#define LS_PG_STATE_SEND	2
#define LS_PG_STATE_SEND_FLUSH	3
#define LS_PG_STATE_RECV	4

static int ls_pg_state = LS_PG_STATE_INIT;

#define LS_PG_QUERY_STRING_LENGTH 1024
typedef struct lspgQueryQueueStruct {
  char qs[LS_PG_QUERY_STRING_LENGTH];				// our queries should all be pretty short as we'll just be calling functions: fixed length here simplifies memory management
  void (*onResponse)( struct lspgQueryQueueStruct *qq, PGresult *pgr);		//
} lspg_query_queue_t;

#define LS_PG_QUERY_QUEUE_LENGTH 16318
static lspg_query_queue_t lspg_query_queue[LS_PG_QUERY_QUEUE_LENGTH];
static unsigned int lspg_query_queue_on    = 0;
static unsigned int lspg_query_queue_off   = 0;
static unsigned int lspg_query_queue_reply = 0;

static PGconn *q = NULL;
static PostgresPollingStatusType lspg_connectPoll_response;
static PostgresPollingStatusType lspg_resetPoll_response;

lspg_query_queue_t *lspg_query_next() {
  if( lspg_query_queue_off == lspg_query_queue_on)
    return NULL;

  return &(lspg_query_queue[(lspg_query_queue_off++) % LS_PG_QUERY_QUEUE_LENGTH]);
}

void lspg_query_reply_next() {
  //
  // this is called only when there is nothing else to do to service
  // the reply: this pop does not return anything.
  //  We use the ...reply_peek function to return the next item in the reply queue
  //

  if( lspg_query_queue_reply != lspg_query_queue_on)
    lspg_query_queue_reply++;
}

lspg_query_queue_t *lspg_query_reply_peek() {
  //
  // Return the next item in the reply queue but don't pop it since we may need it more than once.
  //
  if( lspg_query_queue_reply == lspg_query_queue_on)
    return NULL;

  return &(lspg_query_queue[(lspg_query_queue_reply) % LS_PG_QUERY_QUEUE_LENGTH]);
}

void lspg_query_push( char *s, void (*cb)( lspg_query_queue_t *, PGresult *pgr)) {
  int idx;

  idx = lspg_query_queue_on % LS_PG_QUERY_QUEUE_LENGTH;

  strncpy( lspg_query_queue[idx].qs, s, LS_PG_QUERY_STRING_LENGTH - 1);
  lspg_query_queue[idx].qs[LS_PG_QUERY_STRING_LENGTH - 1] = 0;
  lspg_query_queue[idx].onResponse = cb;
  lspg_query_queue_on++;
};



void lspg_init_motors_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  int i, j;
  uint32_t  motor_number, motor_number_column;
  uint32_t units_column;
  uint32_t u2c_column;
  uint32_t format_column;
  char *sp;
  ls_display_t *lsdp;
  
  motor_number_column = PQfnumber( pgr, "mm_motor");
  units_column        = PQfnumber( pgr, "mm_unit");
  u2c_column          = PQfnumber( pgr, "mm_u2c");
  format_column       = PQfnumber( pgr, "mm_printf");

  if( motor_number_column == -1 || units_column == -1 || u2c_column == -1 || format_column == -1)
    return;

  for( i=0; i<PQntuples( pgr); i++) {

    motor_number = atoi(PQgetvalue( pgr, i, motor_number_column));

    lsdp = NULL;
    for( j=0; j<ls_ndisplays; j++) {
      if( ls_displays[j].motor_num == motor_number) {
	lsdp = &(ls_displays[j]);
	lsdp->units = strdup( PQgetvalue( pgr, i, units_column));
	lsdp->format= strdup( PQgetvalue( pgr, i, format_column));
	lsdp->u2c   = atof(PQgetvalue( pgr, i, u2c_column));
	break;
      }
    }
    if( lsdp == NULL)
      continue;
      

    if( fabs(lsdp->u2c) <= 1.0e-9)
      lsdp->u2c = 1.0;
      
  }
}

void lspg_cmd_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  //
  // Call back funciton assumes query results in zero or more commands to send to the PMAC
  //
  int i;
  char *sp;
  
  for( i=0; i<PQntuples( pgr); i++) {
    sp = PQgetvalue( pgr, i, 0);
    if( sp != NULL && *sp != 0) {
      PmacSockSendline( sp);
      //
      // Keep asking for more until
      // there are no commands left
      // 
      // This should solve a potential problem where
      // more than one command is put on the queue for a given notify.
      //
      lspg_query_push( "select pmac.md2_queue_next()", lspg_cmd_cb);
    }
  }
}

void lsPGService( struct pollfd *evt) {
  //
  // Currently just used to check for notifies
  // Other socket communication is done syncronously
  // Reconsider this if we start using the pmac gather functions
  // since we'll want to be servicing those sockets ASAP
  //

  if( evt->revents & POLLIN) {
    int err;

    if( ls_pg_state == LS_PG_STATE_INIT_POLL) {
      lspg_connectPoll_response = PQconnectPoll( q);
      if( lspg_connectPoll_response == PGRES_POLLING_FAILED) {
	ls_pg_state = LS_PG_STATE_RESET;
      }
      return;
    }

    if( ls_pg_state == LS_PG_STATE_RESET_POLL) {
      lspg_resetPoll_response = PQresetPoll( q);
      if( lspg_resetPoll_response == PGRES_POLLING_FAILED) {
	ls_pg_state = LS_PG_STATE_RESET;
      }
      return;
    }


    //
    // if in IDLE or RECV we need to call consumeInput first
    //
    err = PQconsumeInput( q);
    if( err != 1) {
      wprintw( term_output, "\nconsume input failed: %s\n", PQerrorMessage( q));
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      ls_pg_state == LS_PG_STATE_RESET;
      return;
    }
      

    if( ls_pg_state == LS_PG_STATE_RECV) {
      PGresult *pgr;
      lspg_query_queue_t *qqp;

      //
      // We must call PQgetResult until it returns NULL before sending the next query
      // This implies that only one query can ever be active at a time and our queue
      // management should be simple
      //
      // We should be in the LS_PG_STATE_RECV here
      //

      while( !PQisBusy( q)) {
	pgr = PQgetResult( q);
	if( pgr == NULL) {
	  lspg_query_reply_next();
	  //
	  // we are now done reading the response from the database
	  //
	  ls_pg_state = LS_PG_STATE_IDLE;
	  break;
	} else {
	  ExecStatusType es;

	  qqp = lspg_query_reply_peek();
	  es = PQresultStatus( pgr);

	  if( es != PGRES_COMMAND_OK && es != PGRES_TUPLES_OK) {
	    char *emess;
	    emess = PQresultErrorMessage( pgr);
	    if( emess != NULL && emess[0] != 0) {
	      wprintw( term_output, "\nError from query '%s':\n%s\n", qqp->qs, emess);
	      wnoutrefresh( term_output);
	      wnoutrefresh( term_input);
	      doupdate();
	    }
	  } else {
	    //
	    // Deal with the response
	    //
	    // If the response is likely to take a while we should probably
	    // add a new state and put something in the main look to run the onResponse
	    // routine in the main loop.  For now, though, we only expect very breif onResponse routines
	    //
	    if( qqp != NULL && qqp->onResponse != NULL)
	      qqp->onResponse( qqp, pgr);
	  }
	  PQclear( pgr);
	}
      }
    }

    //
    // Check for notifies regardless of our state
    // Push as many requests as we have notifies.
    //
    {
      PGnotify *pgn;

      while( 1) {
	pgn = PQnotifies( q);
	if( pgn == NULL)
	  break;
	lspg_query_push( "select pmac.md2_queue_next()", lspg_cmd_cb);
	PQfreemem( pgn);
      }
    }
  }

  if( evt->revents & POLLOUT) {

    if( ls_pg_state == LS_PG_STATE_INIT_POLL) {
      lspg_connectPoll_response = PQconnectPoll( q);
      if( lspg_connectPoll_response == PGRES_POLLING_FAILED) {
	ls_pg_state = LS_PG_STATE_RESET;
      }
      return;
    }

    if( ls_pg_state == LS_PG_STATE_RESET_POLL) {
      lspg_resetPoll_response = PQresetPoll( q);
      if( lspg_resetPoll_response == PGRES_POLLING_FAILED) {
	ls_pg_state = LS_PG_STATE_RESET;
      }
      return;
    }


    if( ls_pg_state == LS_PG_STATE_SEND) {
      lspg_query_queue_t *qqp;
      int err;

      qqp = lspg_query_next();
      if( qqp == NULL) {
	//
	// A send without a query?  Should never happen.
	// But at least we shouldn't segfault if it does.
	//
	return;
      }

      if( qqp->qs[0] == 0) {
	//
	// Do we really have to check this case?
	// It would only come up if we stupidly pushed a null query string
	// or ran off the end of the list
	//
	wprintw( term_output, "\nPopped empty query string.  Probably bad things are going on.\n");
	wnoutrefresh( term_output);
	wnoutrefresh( term_input);
	doupdate();

	lspg_query_reply_next();
	ls_pg_state = LS_PG_STATE_IDLE;
      } else {
	err = PQsendQuery( q, qqp->qs);
	if( err == 0) {
	  wprintw( term_output, "\nquery failed: %s\n", PQerrorMessage( q));
	  wnoutrefresh( term_output);
	  wnoutrefresh( term_input);
	  doupdate();

	  lspg_query_reply_next();
	  ls_pg_state == LS_PG_STATE_RESET;
	} else {
	  ls_pg_state = LS_PG_STATE_RECV;
	}
      }

      if( ls_pg_state == LS_PG_STATE_SEND_FLUSH) {
	err = PQflush( q);
	switch( err) {
	case -1:
	  // an error occured
	  wprintw( term_output, "\nflush failed: %s\n", PQerrorMessage( q));
	  wnoutrefresh( term_output);
	  wnoutrefresh( term_input);
	  doupdate();

	  ls_pg_state = LS_PG_STATE_IDLE;
	  //
	  // We should probably reset the connection and start from scratch.  Probably the connection died.
	  //
	  break;
	  
	case 0:
	  // goodness and joy.
	  break;

	case 1:
	  // more sending to do
	  ls_pg_state = LS_PG_STATE_SEND_FLUSH;
	  break;
	}
      }
    }
  }
}


void pg_conn( struct pollfd *fdap) {
  PGresult *pgr;
  int wait_interval = 1;
  int connection_init = 0;
  int i, err;

  if( q == NULL)
    ls_pg_state = LS_PG_STATE_INIT;

  switch( ls_pg_state) {
  case LS_PG_STATE_INIT:
    q = PQconnectStart( "dbname=ls user=lsuser hostaddr=10.1.0.3");
    if( q == NULL) {
      wprintw( term_output, "Out of memory (pg_conn)\n");
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      exit( -1);
    }
    err = PQstatus( q);
    if( err == CONNECTION_BAD) {
      wprintw( term_output, "Trouble connecting to database\n");
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      //
      // TODO: save time of day so we can check that we are not retrying the connection too often
      //
      return;
    }
    err = PQsetnonblocking( q, 1);
    if( err != 0) {
      wprintw( term_output, "Odd, could not set database connection to nonblocking\n");
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
    }

    ls_pg_state = LS_PG_STATE_INIT_POLL;
    lspg_connectPoll_response = PGRES_POLLING_WRITING;
    //
    // set up the connection for poll
    //
    fdap->fd = PQsocket( q);
    break;

  case LS_PG_STATE_INIT_POLL:
    if( lspg_connectPoll_response == PGRES_POLLING_FAILED) {
      PQfinish( q);
      q = NULL;
      ls_pg_state = LS_PG_STATE_INIT;
    } else if( lspg_connectPoll_response == PGRES_POLLING_OK) {
      lspg_query_push( "select * from pmac.md2_getmotors()", lspg_init_motors_cb);
      lspg_query_push( "select pmac.md2_init()", NULL);
      ls_pg_state = LS_PG_STATE_IDLE;
    }
    break;

  case LS_PG_STATE_RESET:
    err = PQresetStart( q);
    if( err == 0) {
      PQfinish( q);
      q = NULL;
      ls_pg_state = LS_PG_STATE_INIT;
    } else {
      ls_pg_state = LS_PG_STATE_RESET_POLL;
      lspg_resetPoll_response = PGRES_POLLING_WRITING;
    }
    break;

  case LS_PG_STATE_RESET_POLL:
    if( lspg_resetPoll_response == PGRES_POLLING_FAILED) {
      PQfinish( q);
      q = NULL;
      ls_pg_state = LS_PG_STATE_INIT;
    } else if( lspg_resetPoll_response == PGRES_POLLING_OK) {
      lspg_query_push( "select * from pmac.md2_getmotors()", lspg_init_motors_cb);
      lspg_query_push( "select pmac.md2_init()", NULL);
      ls_pg_state = LS_PG_STATE_IDLE;
    }
    break;
  }
}

void lspg_init( struct pollfd *fdap) {
  //
  //  make sure these file descriptors are not legal until they've been conneceted
  //
  fdap->fd   = -1;
}


void lspg_next_state( struct pollfd *fdap) {
  //
  // connect to the database
  //
  if( q == NULL ||
      ls_pg_state == LS_PG_STATE_INIT ||
      ls_pg_state == LS_PG_STATE_RESET ||
      ls_pg_state == LS_PG_STATE_INIT_POLL ||
      ls_pg_state == LS_PG_STATE_RESET_POLL)
    pg_conn( fdap);


  if( ls_pg_state == LS_PG_STATE_IDLE && lspg_query_queue_on != lspg_query_queue_off)
    ls_pg_state = LS_PG_STATE_SEND;

  switch( ls_pg_state) {
  case LS_PG_STATE_INIT_POLL:
    if( lspg_connectPoll_response == PGRES_POLLING_WRITING)
      fdap->events = POLLOUT;
    else if( lspg_connectPoll_response == PGRES_POLLING_READING)
      fdap->events = POLLIN;
    else
      fdap->events = 0;
    break;
      
  case LS_PG_STATE_RESET_POLL:
    if( lspg_resetPoll_response == PGRES_POLLING_WRITING)
      fdap->events = POLLOUT;
    else if( lspg_resetPoll_response == PGRES_POLLING_READING)
      fdap->events = POLLIN;
    else
      fdap->events = 0;
    break;

  case LS_PG_STATE_IDLE:
  case LS_PG_STATE_RECV:
    fdap->events = POLLIN;
    break;

  case LS_PG_STATE_SEND:
  case LS_PG_STATE_SEND_FLUSH:
    fdap->events = POLLOUT;
    break;

  default:
    fdap->events = 0;
  }
}
