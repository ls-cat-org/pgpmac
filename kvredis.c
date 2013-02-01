#include <stdio.h>
#include <stdlib.h>
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <poll.h>
#include <postgresql/libpq-fe.h>
#include <string.h>

static redisAsyncContext *subac, *cmdac;


#define LS_PG_QUERY_QUEUE_LENGTH  512
#define LS_PG_QUERY_STRING_LENGTH 512

#define LS_PG_STATE_INIT	-4
#define LS_PG_STATE_INIT_POLL	-3
#define LS_PG_STATE_RESET	-2
#define LS_PG_STATE_RESET_POLL	-1
#define LS_PG_STATE_IDLE	1
#define LS_PG_STATE_SEND	2
#define LS_PG_STATE_SEND_FLUSH	3
#define LS_PG_STATE_RECV	4

static int ls_pg_state = LS_PG_STATE_INIT;	//!< State of the lspg state machine
static struct timeval lspg_time_sent, now;	//!< used to ensure we do not inundate the db server with connection requests
static int kvseq = 0;				//!< used to synchronize pg.kvs and redis

/** Store each query along with it's callback function.
 *  All calls are asynchronous
 */
typedef struct lspgQueryQueueStruct {
  char qs[LS_PG_QUERY_STRING_LENGTH];						//!< our queries should all be pretty short as we'll just be calling functions: fixed length here simplifies memory management
  void (*onResponse)( struct lspgQueryQueueStruct *qq, PGresult *pgr);		//!< Callback function for when a query returns a result
} lspg_query_queue_t;


static lspg_query_queue_t lspg_query_queue[LS_PG_QUERY_QUEUE_LENGTH];	//!< Our query queue
static unsigned int lspg_query_queue_on    = 0;				//!< Next position to add something to the queue
static unsigned int lspg_query_queue_off   = 0;				//!< The last item still being used  (on == off means nothing in queue)
static unsigned int lspg_query_queue_reply = 0;				/**< The current item being digested.  Normally off <= reply <= on.  Corner case of queue wrap arround
									      works because we only increment and compare for equality.
									*/


static PGconn *q = NULL;						//!< Database connector
static PostgresPollingStatusType lspg_connectPoll_response;		//!< Used to determine state while connecting
static PostgresPollingStatusType lspg_resetPoll_response;		//!< Used to determine state while reconnecting
static struct pollfd lspgfd;		//!< our poll info
static struct pollfd subfd;		//!< poll info for redis subscribe channel
static struct pollfd cmdfd;		//!< poll info for redis command channel



void redisDisconnectCB(const redisAsyncContext *ac, int status) {
  if( status == REDIS_OK) {
    printf( "OK, that was fun.\n");
    exit( 0);
  }
  fprintf( stderr, "Opps, Disconnected with status %d\n", status);
  exit( -1);
}

void debugCB( redisAsyncContext *ac, void *reply, void *privdata) {
  static int indentlevel = 0;
  redisReply *r;
  int i;

  r = (redisReply *)reply;

  if( r == NULL) {
    printf( "Null reply.  Odd\n");
    return;
  }
  
  switch( r->type) {
  case REDIS_REPLY_STATUS:
    printf( "%*sSTATUS: %s\n", indentlevel*4,"", r->str);
    break;

  case REDIS_REPLY_ERROR:
    printf( "%*sERROR: %s\n", indentlevel*4, "", r->str);
    break;

  case REDIS_REPLY_INTEGER:
    printf( "%*sInteger: %lld\n", indentlevel*4, "", r->integer);
    break;

  case REDIS_REPLY_NIL:
    printf( "%*s(nil)\n", indentlevel*4, "");
    break;

  case REDIS_REPLY_STRING:
    printf( "%*sSTRING: %s\n", indentlevel*4, "", r->str);
    break;

  case REDIS_REPLY_ARRAY:
    printf( "%*sARRAY of %d elements\n", indentlevel*4, "", (int)r->elements);
    indentlevel++;
    for( i=0; i<r->elements; i++) {
      debugCB( ac, r->element[i], NULL);
    }
    indentlevel--;
    break;
    
  default:
    printf( "%*sUnknown type %d\n", indentlevel*4,"", r->type);
    
  }
}

void addRead( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events |= POLLIN;
}
void delRead( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events &= ~POLLIN;
}
void addWrite( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events |= POLLOUT;
}
void delWrite( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events &= ~POLLOUT;
}
void cleanup( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events &= ~(POLLOUT | POLLIN);
}

void lspg_allkvs_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  int kvname_col, kvvalue_col, kvseq_col, kvdbrtype_col;
  int i;
  int seq;
  char *argv[8];
  
  kvname_col    = PQfnumber( pgr, "rname");
  kvvalue_col   = PQfnumber( pgr, "rvalue");
  kvseq_col     = PQfnumber( pgr, "rseq");
  kvdbrtype_col = PQfnumber( pgr, "rdbrtype");
  
  if( kvname_col == -1 || kvvalue_col == -1 || kvseq_col == -1 || kvdbrtype_col == -1) {
    fprintf( stderr, "lspg_allkvs_cb: bad column number(s)\n");
    return;
  }

  redisAsyncCommand( cmdac, NULL, NULL, "MULTI");
  for( i=0; i<PQntuples( pgr); i++) {
    seq = atoi( PQgetvalue( pgr, i, kvseq_col));
    kvseq = kvseq < seq ? seq : kvseq;

    argv[0] = "HMSET";
    argv[1] = PQgetvalue( pgr, i, kvname_col);
    argv[2] = "VALUE";
    argv[3] = PQgetvalue( pgr, i, kvvalue_col);
    argv[4] = "SEQ";
    argv[5] = PQgetvalue( pgr, i, kvseq_col);
    argv[6] = "DBRTYPE";
    argv[7] = PQgetvalue( pgr, i, kvdbrtype_col);
    redisAsyncCommandArgv( cmdac, NULL, NULL, 8, (const char **)argv, NULL);

    argv[0] = "PUBLISH";
    argv[1] = "REDIS_KV_CONNECTOR";
    argv[2] = PQgetvalue( pgr, i, kvname_col);
    redisAsyncCommandArgv( cmdac, NULL, NULL, 3, (const char **)argv, NULL);
  }

  redisAsyncCommand( cmdac, NULL, NULL, "SET redis.kvseq %d", kvseq);

  redisAsyncCommand( cmdac, NULL, NULL, "EXEC");
  
}



PQnoticeProcessor lspg_notice_processor( void *arg, const char *msg) {
  fprintf( stderr, "lspg: %s", msg);
}


/** Return the next item in the postgresql queue
 *
 * If there is an item left in the queue then it is returned.  Otherwise, NULL is returned.
 */
lspg_query_queue_t *lspg_query_next() {
  lspg_query_queue_t *rtn;
  
  if( lspg_query_queue_off == lspg_query_queue_on)
    // Queue is empty
    rtn = NULL;
  else {
    rtn = &(lspg_query_queue[(lspg_query_queue_off++) % LS_PG_QUERY_QUEUE_LENGTH]); 

  }
  return rtn;
}

/** Remove the oldest item in the queue
 *
 * this is called only when there is nothing else to service
 * the reply: this pop does not return anything.
 *  We use the ...reply_peek function to return the next item in the reply queue
 *
 */
void lspg_query_reply_next() {

  if( lspg_query_queue_reply != lspg_query_queue_on)
    lspg_query_queue_reply++;

}

/** Return the next item in the reply queue but don't pop it since we may need it more than once.
 *  Call lspg_query_reply_next() when done.
 */
lspg_query_queue_t *lspg_query_reply_peek() {
  lspg_query_queue_t *rtn;

  if( lspg_query_queue_reply == lspg_query_queue_on)
    rtn = NULL;
  else
    rtn = &(lspg_query_queue[(lspg_query_queue_reply) % LS_PG_QUERY_QUEUE_LENGTH]);

  return rtn;
}

/** Place a query on the queue
 */
void lspg_query_push(
		     void (*cb)( lspg_query_queue_t *, PGresult *),	/**< [in] Our callback function that deals with the response	*/
		     char *fmt,						/**< [in] Printf style function to generate the query		*/
		     ...						/* Argument for the format string				*/
		     ) {
  int idx;
  va_list arg_ptr;


  //
  // Pause the thread while we service the queue
  //
  if( (lspg_query_queue_on + 1) % LS_PG_QUERY_QUEUE_LENGTH == lspg_query_queue_off % LS_PG_QUERY_QUEUE_LENGTH) {
    fprintf( stderr, "lspg_query_push: queue is full.  Ignoring query \"%s\"\n", fmt);
    return;
  }

  idx = lspg_query_queue_on % LS_PG_QUERY_QUEUE_LENGTH;

  va_start( arg_ptr, fmt);
  vsnprintf( lspg_query_queue[idx].qs, LS_PG_QUERY_STRING_LENGTH-1, fmt, arg_ptr);
  va_end( arg_ptr);

  lspg_query_queue[idx].qs[LS_PG_QUERY_STRING_LENGTH - 1] = 0;
  lspg_query_queue[idx].onResponse = cb;
  lspg_query_queue_on++;

};


/** Receive a result of a query
 */
void lspg_receive() {
  PGresult *pgr;
  lspg_query_queue_t *qqp;
  int err;

  err = PQconsumeInput( q);
  if( err != 1) {
    fprintf( stderr, "consume input failed: %s", PQerrorMessage( q));
    ls_pg_state == LS_PG_STATE_RESET;
    return;
  }

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
	  fprintf( stderr, "Error from query '%s':\n%s", qqp->qs, emess);
	}
      } else {
	//
	// Deal with the response
	//
	// If the response is likely to take awhile we should probably
	// add a new state and put something in the main look to run the onResponse
	// routine in the main loop.  For now, though, we only expect very brief onResponse routines
	//
	if( qqp != NULL && qqp->onResponse != NULL)
	  qqp->onResponse( qqp, pgr);
      }
      PQclear( pgr);
    }
  }
}

/** Connect to the pg server
 */
void lspg_pg_connect() {
  PGresult *pgr;
  int wait_interval = 1;
  int connection_init = 0;
  int i, err;

  if( q == NULL)
    ls_pg_state = LS_PG_STATE_INIT;

  switch( ls_pg_state) {
  case LS_PG_STATE_INIT:

    if( lspg_time_sent.tv_sec != 0) {
      //
      // Reality check: if it's less the about 10 seconds since the last failed attempt
      // the just chill.
      //
      gettimeofday( &now, NULL);
      if( now.tv_sec - lspg_time_sent.tv_sec < 10) {
	return;
      }
    }

    q = PQconnectStart( "dbname=ls user=lsuser hostaddr=10.1.0.3");
    if( q == NULL) {
      fprintf( stderr, "Out of memory (lspg_pg_connect)");
      exit( -1);
    }

    err = PQstatus( q);
    if( err == CONNECTION_BAD) {
      fprintf( stderr, "Trouble connecting to database");

      gettimeofday( &lspg_time_sent, NULL);
      return;
    }
    err = PQsetnonblocking( q, 1);
    if( err != 0) {
      fprintf( stderr, "Odd, could not set database connection to nonblocking");
    }

    ls_pg_state = LS_PG_STATE_INIT_POLL;
    lspg_connectPoll_response = PGRES_POLLING_WRITING;
    //
    // set up the connection for poll
    //
    lspgfd.fd = PQsocket( q);
    break;

  case LS_PG_STATE_INIT_POLL:
    if( lspg_connectPoll_response == PGRES_POLLING_FAILED) {
      PQfinish( q);
      q = NULL;
      ls_pg_state = LS_PG_STATE_INIT;
    } else if( lspg_connectPoll_response == PGRES_POLLING_OK) {
      PQsetNoticeProcessor( q, (PQnoticeProcessor)lspg_notice_processor, NULL);

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
      ls_pg_state = LS_PG_STATE_IDLE;
    }
    break;
  }
}

/** Flush psql output buffer (ie, send the query)
 */
void lspg_flush() {
  int err;

  err = PQflush( q);
  switch( err) {
  case -1:
    // an error occured

    fprintf( stderr, "flush failed: %s\n", PQerrorMessage( q));

    ls_pg_state = LS_PG_STATE_IDLE;
    //
    // We should probably reset the connection and start from scratch.  Probably the connection died.
    //
    break;
	  
  case 0:
    // goodness and joy.
    ls_pg_state = LS_PG_STATE_RECV;
    break;

  case 1:
    // more sending to do
    ls_pg_state = LS_PG_STATE_SEND_FLUSH;
    break;
  }
}

/** Implements our state machine
 *  Does not strictly only set the next state as it also calls some functions
 *  that, perhaps, alters the state mid-function.
 */
void lspg_next_state() {
  //
  // connect to the database
  //
  if( q == NULL ||
      ls_pg_state == LS_PG_STATE_INIT ||
      ls_pg_state == LS_PG_STATE_RESET ||
      ls_pg_state == LS_PG_STATE_INIT_POLL ||
      ls_pg_state == LS_PG_STATE_RESET_POLL)
    lspg_pg_connect( lspgfd);


  if( ls_pg_state == LS_PG_STATE_IDLE && lspg_query_queue_on != lspg_query_queue_off)
    ls_pg_state = LS_PG_STATE_SEND;

  switch( ls_pg_state) {
  case LS_PG_STATE_INIT_POLL:
    if( lspg_connectPoll_response == PGRES_POLLING_WRITING)
      lspgfd.events = POLLOUT;
    else if( lspg_connectPoll_response == PGRES_POLLING_READING)
      lspgfd.events = POLLIN;
    else
      lspgfd.events = 0;
    break;
      
  case LS_PG_STATE_RESET_POLL:
    if( lspg_resetPoll_response == PGRES_POLLING_WRITING)
      lspgfd.events = POLLOUT;
    else if( lspg_resetPoll_response == PGRES_POLLING_READING)
      lspgfd.events = POLLIN;
    else
      lspgfd.events = 0;
    break;

  case LS_PG_STATE_IDLE:
  case LS_PG_STATE_RECV:
    lspgfd.events = POLLIN;
    break;

  case LS_PG_STATE_SEND:
  case LS_PG_STATE_SEND_FLUSH:
    lspgfd.events = POLLOUT;
    break;

  default:
    lspgfd.events = 0;
  }
}


/** send the next queued query to the DB server
 */
void lspg_send_next_query() {
  //
  // Normally we should be in the "send" state
  // but we can also send if we are servicing
  // a reply
  //

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
    // It would only come up if we stupidly pushed an empty query string
    // or ran off the end of the queue
    //
    fprintf( stderr, "Popped empty query string.  Probably bad things are going on.\n");

    lspg_query_reply_next();
    ls_pg_state = LS_PG_STATE_IDLE;
  } else {
    err = PQsendQuery( q, qqp->qs);
    if( err == 0) {
      fprintf( stderr, "query failed: %s\n", PQerrorMessage( q));

      //
      // Don't wait for a reply, just reset the connection
      //
      lspg_query_reply_next();
      ls_pg_state == LS_PG_STATE_RESET;
    } else {
      ls_pg_state = LS_PG_STATE_SEND_FLUSH;
    }
  }
}

/** I/O control to/from the postgresql server
 */
void lspg_pg_service(
		     struct pollfd *evt			/**<[in] The pollfd object that we are responding to	*/
		     ) {
  //
  // Currently just used to check for notifies
  // Other socket communication is done syncronously
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
    if( ls_pg_state == LS_PG_STATE_IDLE) {
      err = PQconsumeInput( q);
      if( err != 1) {
	fprintf( stderr, "consume input failed: %s", PQerrorMessage( q));
	ls_pg_state == LS_PG_STATE_RESET;
	return;
      }
    }      

    if( ls_pg_state == LS_PG_STATE_RECV) {
      lspg_receive();
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
	
	lspg_query_push( lspg_allkvs_cb, "EXECUTE redis_kv_update(%d)", kvseq);

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
      lspg_send_next_query();
    }

    if( ls_pg_state == LS_PG_STATE_SEND_FLUSH) {
      lspg_flush();
    }
  }
}


void fd_service( struct pollfd *evt) {
  if( evt->fd == subac->c.fd) {
    if( evt->revents & POLLIN)
      redisAsyncHandleRead( subac);
    if( evt->revents & POLLOUT)
      redisAsyncHandleWrite( subac);
  }
  if( evt->fd == cmdac->c.fd) {
    if( evt->revents & POLLIN)
      redisAsyncHandleRead( cmdac);
    if( evt->revents & POLLOUT)
      redisAsyncHandleWrite( cmdac);
  }
  if( q && evt->fd == PQsocket( q))
    lspg_pg_service( evt);
}



main() {
  static struct pollfd fda[3];
  static int nfda = 0;
  int pollrtn;
  int poll_timeout_ms;
  int i;

  subac = redisAsyncConnect("127.0.0.1", 6379);
  if( subac->err) {
    fprintf( stderr, "Error: %s\n", subac->errstr);
    exit( -1);
  }

  cmdac = redisAsyncConnect("127.0.0.1", 6379);
  if( cmdac->err) {
    fprintf( stderr, "Error: %s\n", cmdac->errstr);
    exit( -1);
  }

  if( redisAsyncSetDisconnectCallback( subac, redisDisconnectCB) == REDIS_ERR) {
    fprintf( stderr, "Error: could not set disconnect callback\n");
    exit( -1);
  }

  if( redisAsyncSetDisconnectCallback( cmdac, redisDisconnectCB) == REDIS_ERR) {
    fprintf( stderr, "Error: could not set disconnect callback\n");
    exit( -1);
  }

  // Set up redis events
  //
  subfd.fd           = subac->c.fd;
  subfd.events       = 0;
  subac->ev.data     = &subfd;
  subac->ev.addRead  = addRead;
  subac->ev.delRead  = delRead;
  subac->ev.addWrite = addWrite;
  subac->ev.delWrite = delWrite;
  subac->ev.cleanup  = cleanup;

  cmdfd.fd           = cmdac->c.fd;
  cmdfd.events       = 0;
  cmdac->ev.data     = &cmdfd;
  cmdac->ev.addRead  = addRead;
  cmdac->ev.delRead  = delRead;
  cmdac->ev.addWrite = addWrite;
  cmdac->ev.delWrite = delWrite;
  cmdac->ev.cleanup  = cleanup;


  lspgfd.fd = -1;

  if( redisAsyncCommand( cmdac, NULL, NULL, "KEYS *") == REDIS_ERR) {
    fprintf( stderr, "Error sending KEYS command\n");
    exit( -1);
  }

  if( redisAsyncCommand( subac, debugCB, NULL, "PSUBSCRIBE MD2* UI* mk_pgpmac_redis") == REDIS_ERR) {
    fprintf( stderr, "Error sending PSUBSCRIBE command\n");
    exit( -1);
  }

  lspg_query_push( NULL, "PREPARE redis_kv_update ( int) AS SELECT * FROM px.redis_kv_update($1)");


  lspg_query_push( lspg_allkvs_cb, "SELECT * FROM px.redis_kv_init()");
  lspg_query_push( NULL, "LISTEN REDIS_KV_CONNECTOR");

  while( 1) {
    nfda = 0;
    if( subfd.fd != -1) {
      fda[nfda].fd      = subfd.fd;
      fda[nfda].events  = subfd.events;
      fda[nfda].revents = 0;

      nfda++;
    }
    if( cmdfd.fd != -1) {
      fda[nfda].fd      = cmdfd.fd;
      fda[nfda].events  = cmdfd.events;
      fda[nfda].revents = 0;
      
      nfda++;
    }
    poll_timeout_ms = -1;

    lspg_next_state();

    if( lspgfd.fd == -1) {
      //
      // Here a connection to the database is not established.
      // Periodicaly try again.  Should possibly arrange to reconnect
      // to signalfd but that's unlikely to be nessesary.
      //
      poll_timeout_ms = 10000;
    } else {
      //
      // Arrange to peacfully do nothing until either the pg server sends us something
      // or someone pushs something onto our queue
      //
      fda[nfda].fd      = lspgfd.fd;
      fda[nfda].events  = lspgfd.events;
      fda[nfda].revents = 0;
      nfda++;
      poll_timeout_ms = -1;
    }


    pollrtn = poll( fda, nfda, poll_timeout_ms);

    for( i=0; i<nfda; i++) {
      if( fda[i].revents) {
	fd_service( &(fda[i]));
      }
    }
  }
}
