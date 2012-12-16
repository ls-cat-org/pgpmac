/*! \file lspg.c
 *  \brief Postgresql support for the LS-CAT pgpmac project
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 *
\details

<pre>
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
</pre>
*/



#include "pgpmac.h"


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

static pthread_t lspg_thread;		//!< our worker thread
static pthread_mutex_t lspg_queue_mutex;	//!< keep the queue from getting tangled
static pthread_cond_t  lspg_queue_cond;	//!< keeps the queue from overflowing
static struct pollfd lspgfd;		//!< our poll info


/** Store each query along with it's callback function.
 *  All calls are asynchronous
 */
typedef struct lspgQueryQueueStruct {
  char qs[LS_PG_QUERY_STRING_LENGTH];						//!< our queries should all be pretty short as we'll just be calling functions: fixed length here simplifies memory management
  void (*onResponse)( struct lspgQueryQueueStruct *qq, PGresult *pgr);		//!< Callback function for when a query returns a result
} lspg_query_queue_t;

/** Queue length should be long enough that we do not ordinarly bump into the end
 *  We should be safe as long as the thread the adds stuff to the queue is not
 *  the one that removes it.  (And we can tolerate the adding thread being paused.)
 */
#define LS_PG_QUERY_QUEUE_LENGTH 16384
static lspg_query_queue_t lspg_query_queue[LS_PG_QUERY_QUEUE_LENGTH];	//!< Our query queue
static unsigned int lspg_query_queue_on    = 0;				//!< Next position to add something to the queue
static unsigned int lspg_query_queue_off   = 0;				//!< The last item still being used  (on == off means nothing in queue)
static unsigned int lspg_query_queue_reply = 0;				/**< The current item being digested.  Normally off <= reply <= on.  Corner case of queue wrap arround
									      works because we only increment and compare for equality.
									*/

static PGconn *q = NULL;						//!< Database connector
static PostgresPollingStatusType lspg_connectPoll_response;		//!< Used to determine state while connecting
static PostgresPollingStatusType lspg_resetPoll_response;		//!< Used to determine state while reconnecting

lspg_nextshot_t  lspg_nextshot;						//!< the nextshot object
lspg_getcenter_t lspg_getcenter;					//!< the getcenter object

/** Return the next item in the postgresql queue
 *
 * If there is an item left in the queue then it is returned.  Otherwise, NULL is returned.
 */
lspg_query_queue_t *lspg_query_next() {
  lspg_query_queue_t *rtn;
  
  pthread_mutex_lock( &lspg_queue_mutex);

  if( lspg_query_queue_off == lspg_query_queue_on)
    // Queue is empty
    rtn = NULL;
  else {
    rtn = &(lspg_query_queue[(lspg_query_queue_off++) % LS_PG_QUERY_QUEUE_LENGTH]); 
    pthread_cond_signal( &lspg_queue_cond);
  }
  pthread_mutex_unlock( &lspg_queue_mutex);

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

  pthread_mutex_lock( &lspg_queue_mutex);

  if( lspg_query_queue_reply != lspg_query_queue_on)
    lspg_query_queue_reply++;

  pthread_mutex_unlock( &lspg_queue_mutex);
}

/** Return the next item in the reply queue but don't pop it since we may need it more than once.
 *  Call lspg_query_reply_next() when done.
 */
lspg_query_queue_t *lspg_query_reply_peek() {
  lspg_query_queue_t *rtn;

  pthread_mutex_lock( &lspg_queue_mutex);

  if( lspg_query_queue_reply == lspg_query_queue_on)
    rtn = NULL;
  else
    rtn = &(lspg_query_queue[(lspg_query_queue_reply) % LS_PG_QUERY_QUEUE_LENGTH]);

  pthread_mutex_unlock( &lspg_queue_mutex);
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

  pthread_mutex_lock( &lspg_queue_mutex);

  //
  // Pause the thread while we service the queue
  //
  while( lspg_query_queue_on + 1 == lspg_query_queue_off) {
    pthread_cond_wait( &lspg_queue_cond, &lspg_queue_mutex);
  }

  idx = lspg_query_queue_on % LS_PG_QUERY_QUEUE_LENGTH;

  va_start( arg_ptr, fmt);
  vsnprintf( lspg_query_queue[idx].qs, LS_PG_QUERY_STRING_LENGTH-1, fmt, arg_ptr);
  va_end( arg_ptr);

  lspg_query_queue[idx].qs[LS_PG_QUERY_STRING_LENGTH - 1] = 0;
  lspg_query_queue[idx].onResponse = cb;
  lspg_query_queue_on++;

  pthread_kill( lspg_thread, SIGUSR1);
  pthread_mutex_unlock( &lspg_queue_mutex);
};

/** returns a null terminated list of strings parsed from postgresql array
 */
char **lspg_array2ptrs( char *a) {
  char **rtn, *sp, *acums;
  int i, n, inquote, havebackslash, rtni;;
  int mxsz;
  
  inquote       = 0;
  havebackslash = 0;

  // Despense with the null input condition before we complicate the code below
  if( a == NULL || a[0] == 0)
    return NULL;

  // Count the maximum number of strings
  // Actual number will be less if there are quoted commas
  //
  n = 1;
  for( i=0; a[i]; i++) {
    if( a[i] == ',')
      n++;
  }
  //
  // The maximum size of any string is the length of a (+1)
  //
  mxsz = strlen(a) + 1;

  // This is the accumulation string to make up the array elements
  acums = (char *)calloc( mxsz, sizeof( char));
  if( acums == NULL) {
    // TODO: print or otherwise log this condition
    // out of memory
    exit( 1);
  }
  
  //
  // allocate storage for the pointer array and the null terminator
  //
  rtn = (char **)calloc( n+1, sizeof( char *));
  if( rtn == NULL) {
    // TODO: print or otherwise log this condition
    // out of memory
    exit( 1);
  }
  rtni = 0;
  
  // Go through and create the individual strings
  sp = acums;
  *sp = 0;
  if( a[0] != '{') {
    // oh no!  This isn't an array after all!
    // Zounds!
    return NULL;
  }
  inquote = 0;
  havebackslash = 0;
  for( i=1; a[i] != 0; i++) {
    switch( a[i]) {
    case '"':
      if( havebackslash) {
	// a quoted quote.  Cool
	//
	*(sp++) = a[i];
	*sp = 0;
	havebackslash = 0;
      } else {
	// Toggle the flag
	inquote = 1 - inquote;
      }
      break;

    case '\\':
      if( havebackslash) {
	*(sp++) = a[i];
	*sp = 0;
	havebackslash = 0;
      } else {
	havebackslash = 1;
      }
      break;

    case ',':
      if( inquote || havebackslash) {
	*(sp++) = a[i];
	*sp = 0;
	havebackslash = 0;
      } else {
	rtn[rtni++] = strdup( acums);
	sp = acums;
      }
      break;
      
    case '}':
      if( inquote || havebackslash) {
	*(sp++) = a[i];
	*sp = 0;
	havebackslash = 0;
      } else {
	rtn[rtni++] = strdup( acums);
	rtn[rtni]   = NULL;
	return( rtn);
      }
      break;

    default:
      *(sp++) = a[i];
      *sp = 0;
      havebackslash = 0;
    }
  }
  //
  // Getting here means the final '}' was missing
  // Probably we should throw an error or log it or something.
  //
  rtn[rtni++] = strdup( acums);
  rtn[rtni]   = NULL;
  return( rtn);
}

/** Motor initialization callback
 */
void lspg_init_motors_cb(
			 lspg_query_queue_t *qqp,		/**< [in] The query queue item used to call us		*/
			 PGresult *pgr				/**< [in] The postgresql result object			*/
			 ) {
  int i, j;
  //  uint32_t home_column;
  uint32_t name_column;
  char *sp;
  lspmac_motor_t *lsdp;
  
  name_column              = PQfnumber( pgr, "mm_name");
  if( name_column == -1)
    return;

  //  home_column		   = PQfnumber( pgr, "mm_home");

  for( i=0; i<PQntuples( pgr); i++) {

    lsdp = NULL;
    for( j=0; j<lspmac_nmotors; j++) {
      if( strcmp(lspmac_motors[j].name, PQgetvalue( pgr, i, name_column)) == 0) {
	lsdp                    = &(lspmac_motors[j]);

	//	lsdp->home              = lspg_array2ptrs( PQgetvalue( pgr, i, home_column));
	//	lsdp->lspg_initialized  = 1;
	break;
      }
    }
    if( lsdp == NULL)
      continue;
  }
}

/** Zoom motor look up table callback
 */
void lspg_zoom_lut_cb(
		      lspg_query_queue_t *qqp,		/**< [in] the queue item responsible for calling us	*/
		      PGresult *pgr			/**< [in] The Postgresql result object			*/
		      ) {
  int i;
  
  pthread_mutex_lock( &(zoom->mutex));

  zoom->nlut = PQntuples( pgr)/2;
  zoom->lut  = calloc( 2*zoom->nlut, sizeof(double));
  if( zoom->lut == NULL) {
    lslogging_log_message( "Out of memmory (lspg_zoom_lut_cb)");
    pthread_mutex_unlock( &(zoom->mutex));
    return;
  }
  
  for( i=0; i<PQntuples( pgr); i++) {
    zoom->lut[i] = strtod( PQgetvalue( pgr, i, 0), NULL);
  }

  pthread_mutex_unlock( &(zoom->mutex));
}

void lspg_scint_lut_cb(
			lspg_query_queue_t *qqp,		/**< [in] Our query			*/
			PGresult *pgr				/**< [in] Our result object		*/
			) {
  int i;
  pthread_mutex_lock( &(fscint->mutex));
  
  fscint->nlut = PQntuples( pgr)/2;
  fscint->lut  = calloc( 2*fscint->nlut, sizeof( double));
  if( fscint->lut == NULL) {
    lslogging_log_message( "lspg_scint_lut_cb: Out of memory");
    pthread_mutex_unlock( &(fscint->mutex));
  }
  
  for( i=0; i<PQntuples( pgr); i++) {
    fscint->lut[i] = strtod( PQgetvalue( pgr, i, 0), NULL);
  }

  pthread_mutex_unlock( &(fscint->mutex));
}

/** Front Light Lookup table query callback
 * Install the lookup table for the Front Light
 */
void lspg_flight_lut_cb(
			lspg_query_queue_t *qqp,		/**< [in] Our query			*/
			PGresult *pgr				/**< [in] Our result object		*/
			) {
  int i;
  
  pthread_mutex_lock( &(flight->mutex));

  flight->nlut = PQntuples( pgr)/2;
  flight->lut  = calloc( 2*flight->nlut, sizeof(double));
  if( flight->lut == NULL) {
    lslogging_log_message( "Out of memmory (lspg_flight_lut_cb)");
    pthread_mutex_unlock( &(flight->mutex));
    return;
  }
  
  for( i=0; i<PQntuples( pgr); i++) {
    flight->lut[i] = strtod( PQgetvalue( pgr, i, 0), NULL);
  }

  pthread_mutex_unlock( &(flight->mutex));

}


/** Back Light Lookup Table Callback
 *  Install the lookup table for the Back Light
 */
void lspg_blight_lut_cb(
			lspg_query_queue_t *qqp,		/**< [in] Our query			*/
			PGresult *pgr				/**< [in] The query's result		*/
			) {
  int i;
  
  pthread_mutex_lock( &(blight->mutex));

  blight->nlut = PQntuples( pgr)/2;
  blight->lut  = calloc( 2*blight->nlut, sizeof(double));
  if( blight->lut == NULL) {
    lslogging_log_message( "Out of memmory (lspg_blight_lut_cb)");
    pthread_mutex_unlock( &(blight->mutex));
    return;
  }
  
  for( i=0; i<PQntuples( pgr); i++) {
    blight->lut[i] = strtod( PQgetvalue( pgr, i, 0), NULL);
  }

  pthread_mutex_unlock( &(blight->mutex));

}



/** Next Shot Callback.
 *  This is a long and tedious routine as there are a large
 *  number of variables returned.
 *  Suck it up.
 *  Return with the global variable lspg_nextshot set.
 */
void lspg_nextshot_cb(
		      lspg_query_queue_t *qqp,		/**< [in] Our nextshot query			*/
		      PGresult *pgr			/**< [in] result of the query			*/
		      ) {
  static int got_col_nums=0;
  static int
    dsdir_c, dspid_c, dsowidth_c, dsoscaxis_c, dsexp_c, skey_c, sstart_c, sfn_c, dsphi_c,
    dsomega_c, dskappa_c, dsdist_c, dsnrg_c, dshpid_c, cx_c, cy_c, ax_c, ay_c, az_c,
    active_c, sindex_c, stype_c,
    dsowidth2_c, dsoscaxis2_c, dsexp2_c, sstart2_c, dsphi2_c, dsomega2_c, dskappa2_c, dsdist2_c, dsnrg2_c,
    cx2_c, cy2_c, ax2_c, ay2_c, az2_c, active2_c, sindex2_c, stype2_c;
  
  pthread_mutex_lock( &(lspg_nextshot.mutex));

  lspg_nextshot.no_rows_returned = PQntuples( pgr) <= 0;
  if( lspg_nextshot.no_rows_returned) {
    lspg_nextshot.new_value_ready = 1;
    pthread_cond_signal( &(lspg_nextshot.cond));
    pthread_mutex_unlock( &(lspg_nextshot.mutex));
    return;			// I guess there was no shot after all
  }

  if( got_col_nums == 0) {
    dsdir_c      = PQfnumber( pgr, "dsdir");
    dspid_c      = PQfnumber( pgr, "dspid");
    dsowidth_c   = PQfnumber( pgr, "dsowidth");
    dsoscaxis_c  = PQfnumber( pgr, "dsoscaxis");
    dsexp_c      = PQfnumber( pgr, "dsexp");
    skey_c       = PQfnumber( pgr, "skey");
    sstart_c     = PQfnumber( pgr, "sstart");
    sfn_c        = PQfnumber( pgr, "sfn");
    dsphi_c      = PQfnumber( pgr, "dsphi");
    dsomega_c    = PQfnumber( pgr, "dsomega");
    dskappa_c    = PQfnumber( pgr, "dskappa");
    dsdist_c     = PQfnumber( pgr, "dsdist");
    dsnrg_c      = PQfnumber( pgr, "dsnrg");
    dshpid_c     = PQfnumber( pgr, "dshpid");
    cx_c         = PQfnumber( pgr, "cx");
    cy_c         = PQfnumber( pgr, "cy");
    ax_c         = PQfnumber( pgr, "ax");
    ay_c         = PQfnumber( pgr, "ay");
    az_c         = PQfnumber( pgr, "az");
    active_c     = PQfnumber( pgr, "active");
    sindex_c     = PQfnumber( pgr, "sindex");
    stype_c      = PQfnumber( pgr, "stype");
    dsowidth2_c  = PQfnumber( pgr, "dsowidth2");
    dsoscaxis2_c = PQfnumber( pgr, "dsoscaxis2");
    dsexp2_c     = PQfnumber( pgr, "dsexp2");
    sstart2_c    = PQfnumber( pgr, "sstart2");
    dsphi2_c     = PQfnumber( pgr, "dsphi2");
    dsomega2_c   = PQfnumber( pgr, "dsomega2");
    dskappa2_c   = PQfnumber( pgr, "dskappa2");
    dsdist2_c    = PQfnumber( pgr, "dsdist2");
    dsnrg2_c     = PQfnumber( pgr, "dsnrg2");
    cx2_c        = PQfnumber( pgr, "cx2");
    cy2_c        = PQfnumber( pgr, "cy2");
    ax2_c        = PQfnumber( pgr, "ax2");
    ay2_c        = PQfnumber( pgr, "ay2");
    az2_c        = PQfnumber( pgr, "az2");
    active2_c    = PQfnumber( pgr, "active2");
    sindex2_c    = PQfnumber( pgr, "sindex2");
    stype2_c     = PQfnumber( pgr, "stype2");
    
    got_col_nums = 1;
  }


  //
  // NULL string values come back as empty strings
  // Mark the null flag but allocate the empty string anyway
  //

  lspg_nextshot.dsdir_isnull = PQgetisnull( pgr, 0, dsdir_c);
  if( lspg_nextshot.dsdir != NULL)
    free( lspg_nextshot.dsdir);
  lspg_nextshot.dsdir = strdup( PQgetvalue( pgr, 0, dsdir_c));

  lspg_nextshot.dspid_isnull = PQgetisnull( pgr, 0, dspid_c);
  if( lspg_nextshot.dspid != NULL)
    free( lspg_nextshot.dspid);
  lspg_nextshot.dspid = strdup( PQgetvalue( pgr, 0, dspid_c));

  lspg_nextshot.dsoscaxis_isnull = PQgetisnull( pgr, 0, dsoscaxis_c);
  if( lspg_nextshot.dsoscaxis != NULL)
    free( lspg_nextshot.dsoscaxis);
  lspg_nextshot.dsoscaxis = strdup( PQgetvalue( pgr, 0, dsoscaxis_c));

  lspg_nextshot.dsoscaxis2_isnull = PQgetisnull( pgr, 0, dsoscaxis2_c);
  if( lspg_nextshot.dsoscaxis2 != NULL)
    free( lspg_nextshot.dsoscaxis2);
  lspg_nextshot.dsoscaxis2 = strdup( PQgetvalue( pgr, 0, dsoscaxis2_c));

  lspg_nextshot.sfn_isnull = PQgetisnull(pgr, 0, sfn_c);
  if( lspg_nextshot.sfn != NULL)
    free( lspg_nextshot.sfn);
  lspg_nextshot.sfn = strdup( PQgetvalue( pgr, 0, sfn_c));

  lspg_nextshot.stype_isnull = PQgetisnull( pgr, 0, stype_c);
  if( lspg_nextshot.stype != NULL)
    free( lspg_nextshot.stype);
  lspg_nextshot.stype = strdup( PQgetvalue( pgr, 0, stype_c));

  lspg_nextshot.stype2_isnull = PQgetisnull( pgr, 0, stype2_c);
  if( lspg_nextshot.stype2 != NULL)
    free( lspg_nextshot.stype2);
  lspg_nextshot.stype2 = strdup( PQgetvalue( pgr, 0, stype2_c));

  //
  // Probably shouldn't try to convert null number values
  //
  lspg_nextshot.dsowidth_isnull = PQgetisnull( pgr, 0, dsowidth_c);
  if( lspg_nextshot.dsowidth_isnull == 0)
    lspg_nextshot.dsowidth = atof( PQgetvalue( pgr,0, dsowidth_c));

  lspg_nextshot.dsexp_isnull = PQgetisnull( pgr, 0, dsexp_c);
  if( lspg_nextshot.dsexp_isnull == 0)
    lspg_nextshot.dsexp    = atof( PQgetvalue( pgr,0, dsexp_c));

  lspg_nextshot.sstart_isnull = PQgetisnull( pgr, 0, sstart_c);
  if( lspg_nextshot.sstart_isnull == 0)
    lspg_nextshot.sstart   = atof( PQgetvalue( pgr,0, sstart_c));

  lspg_nextshot.dsphi_isnull = PQgetisnull( pgr, 0, dsphi_c);
  if( lspg_nextshot.dsphi_isnull == 0)
    lspg_nextshot.dsphi    = atof( PQgetvalue( pgr,0, dsphi_c));

  lspg_nextshot.dsomega_isnull = PQgetisnull( pgr, 0, dsomega_c);
  if( lspg_nextshot.dsomega_isnull == 0)
    lspg_nextshot.dsomega  = atof( PQgetvalue( pgr,0, dsomega_c));

  lspg_nextshot.dskappa_isnull = PQgetisnull( pgr, 0, dskappa_c);
  if( lspg_nextshot.dskappa_isnull == 0)
    lspg_nextshot.dskappa  = atof( PQgetvalue( pgr,0, dskappa_c));

  lspg_nextshot.dsdist_isnull = PQgetisnull( pgr, 0, dsdist_c);
  if( lspg_nextshot.dsdist_isnull == 0)
    lspg_nextshot.dsdist   = atof( PQgetvalue( pgr,0, dsdist_c));

  lspg_nextshot.dsnrg_isnull = PQgetisnull( pgr, 0, dsnrg_c);
  if( lspg_nextshot.dsnrg_isnull == 0)
    lspg_nextshot.dsnrg    = atof( PQgetvalue( pgr,0, dsnrg_c));

  lspg_nextshot.cx_isnull = PQgetisnull( pgr, 0, cx_c);
  if( lspg_nextshot.cx_isnull == 0)
    lspg_nextshot.cx       = atof( PQgetvalue( pgr,0, cx_c));

  lspg_nextshot.cy_isnull = PQgetisnull( pgr, 0, cy_c);
  if( lspg_nextshot.cy_isnull == 0)
    lspg_nextshot.cy       = atof( PQgetvalue( pgr,0, cy_c));

  lspg_nextshot.ax_isnull = PQgetisnull( pgr, 0, ax_c);
  if( lspg_nextshot.ax_isnull == 0)
    lspg_nextshot.ax       = atof( PQgetvalue( pgr,0, ax_c));

  lspg_nextshot.ay_isnull = PQgetisnull( pgr, 0, ay_c);
  if( lspg_nextshot.ay_isnull == 0)
    lspg_nextshot.ay       = atof( PQgetvalue( pgr,0, ay_c));

  lspg_nextshot.az_isnull = PQgetisnull( pgr, 0, az_c);
  if( lspg_nextshot.az_isnull == 0)
    lspg_nextshot.az       = atof( PQgetvalue( pgr,0, az_c));
  
  lspg_nextshot.active_isnull = PQgetisnull( pgr, 0, active_c);
  if( lspg_nextshot.active_isnull == 0)
    lspg_nextshot.active = atoi( PQgetvalue( pgr, 0, active_c));

  lspg_nextshot.sindex_isnull = PQgetisnull( pgr, 0, sindex_c);
  if( lspg_nextshot.sindex_isnull == 0)
    lspg_nextshot.sindex = atoi( PQgetvalue( pgr, 0, sindex_c));

  lspg_nextshot.dshpid_isnull = PQgetisnull( pgr, 0, dshpid_c);
  if( lspg_nextshot.dshpid_isnull == 0)
    lspg_nextshot.dshpid = atoi( PQgetvalue( pgr, 0, dshpid_c));
  
  lspg_nextshot.skey_isnull = PQgetisnull( pgr, 0, skey_c);
  if( lspg_nextshot.skey_isnull == 0)
    lspg_nextshot.skey   = atoll( PQgetvalue( pgr, 0, skey_c));

  lspg_nextshot.dsowidth2_isnull = PQgetisnull( pgr, 0, dsowidth2_c);
  if( lspg_nextshot.dsowidth2_isnull == 0)
    lspg_nextshot.dsowidth2 = atof( PQgetvalue( pgr,0, dsowidth2_c));

  lspg_nextshot.dsexp2_isnull = PQgetisnull( pgr, 0, dsexp2_c);
  if( lspg_nextshot.dsexp2_isnull == 0)
    lspg_nextshot.dsexp2    = atof( PQgetvalue( pgr,0, dsexp2_c));

  lspg_nextshot.sstart2_isnull = PQgetisnull( pgr, 0, sstart2_c);
  if( lspg_nextshot.sstart2_isnull == 0)
    lspg_nextshot.sstart2   = atof( PQgetvalue( pgr,0, sstart2_c));

  lspg_nextshot.dsphi2_isnull = PQgetisnull( pgr, 0, dsphi2_c);
  if( lspg_nextshot.dsphi2_isnull == 0)
    lspg_nextshot.dsphi2    = atof( PQgetvalue( pgr,0, dsphi2_c));

  lspg_nextshot.dsomega2_isnull = PQgetisnull( pgr, 0, dsomega2_c);
  if( lspg_nextshot.dsomega2_isnull == 0)
    lspg_nextshot.dsomega2  = atof( PQgetvalue( pgr,0, dsomega2_c));

  lspg_nextshot.dskappa2_isnull = PQgetisnull( pgr, 0, dskappa2_c);
  if( lspg_nextshot.dskappa2_isnull == 0)
    lspg_nextshot.dskappa2  = atof( PQgetvalue( pgr,0, dskappa2_c));

  lspg_nextshot.dsdist2_isnull = PQgetisnull( pgr, 0, dsdist2_c);
  if( lspg_nextshot.dsdist2_isnull == 0)
    lspg_nextshot.dsdist2   = atof( PQgetvalue( pgr,0, dsdist2_c));

  lspg_nextshot.dsnrg2_isnull = PQgetisnull( pgr, 0, dsnrg2_c);
  if( lspg_nextshot.dsnrg2_isnull == 0)
    lspg_nextshot.dsnrg2    = atof( PQgetvalue( pgr,0, dsnrg2_c));

  lspg_nextshot.cx2_isnull = PQgetisnull( pgr, 0, cx2_c);
  if( lspg_nextshot.cx2_isnull == 0)
    lspg_nextshot.cx2       = atof( PQgetvalue( pgr,0, cx2_c));

  lspg_nextshot.cy2_isnull = PQgetisnull( pgr, 0, cy2_c);
  if( lspg_nextshot.cy2_isnull == 0)
    lspg_nextshot.cy2       = atof( PQgetvalue( pgr,0, cy2_c));

  lspg_nextshot.ax2_isnull = PQgetisnull( pgr, 0, ax2_c);
  if( lspg_nextshot.ax2_isnull == 0)
    lspg_nextshot.ax2       = atof( PQgetvalue( pgr,0, ax2_c));

  lspg_nextshot.ay2_isnull = PQgetisnull( pgr, 0, ay2_c);
  if( lspg_nextshot.ay2_isnull == 0)
    lspg_nextshot.ay2       = atof( PQgetvalue( pgr,0, ay2_c));

  lspg_nextshot.az2_isnull = PQgetisnull( pgr, 0, az2_c);
  if( lspg_nextshot.az2_isnull == 0)
    lspg_nextshot.az2       = atof( PQgetvalue( pgr,0, az2_c));
  
  lspg_nextshot.active2_isnull = PQgetisnull( pgr, 0, active2_c);
  if( lspg_nextshot.active2_isnull == 0)
    lspg_nextshot.active2 = atoi( PQgetvalue( pgr, 0, active2_c));

  lspg_nextshot.sindex2_isnull = PQgetisnull( pgr, 0, sindex2_c);
  if( lspg_nextshot.sindex2_isnull == 0)
    lspg_nextshot.sindex2 = atoi( PQgetvalue( pgr, 0, sindex2_c));

  lspg_nextshot.new_value_ready = 1;

  pthread_cond_signal( &(lspg_nextshot.cond));
  pthread_mutex_unlock( &(lspg_nextshot.mutex));

}

/** Initialize the nextshot variable, mutex, and condition
 */
void lspg_nextshot_init() {
  memset( &lspg_nextshot, 0, sizeof( lspg_nextshot));
  pthread_mutex_init( &(lspg_nextshot.mutex), NULL);
  pthread_cond_init( &(lspg_nextshot.cond), NULL);
}

/** Queue up a nextshot query
 */
void lspg_nextshot_call() {
  pthread_mutex_lock( &(lspg_nextshot.mutex));
  lspg_nextshot.new_value_ready = 0;
  pthread_mutex_unlock( &(lspg_nextshot.mutex));
  
  lspg_query_push( lspg_nextshot_cb, "SELECT * FROM px.nextshot()");
}

/** Wait for the next shot query to get processed
 */
void lspg_nextshot_wait() {
  pthread_mutex_lock( &(lspg_nextshot.mutex));
  while( lspg_nextshot.new_value_ready == 0)
    pthread_cond_wait( &(lspg_nextshot.cond), &(lspg_nextshot.mutex));
}

/** Called when the next shot query has been processed
 */
void lspg_nextshot_done() {
  pthread_mutex_unlock( &(lspg_nextshot.mutex));
}

/** Object that implements detector / spindle timing
 *  We use database locks for exposure control and
 *  this implements the md2 portion of this handshake
 */
typedef struct lspg_wait_for_detector_struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int new_value_ready;
} lspg_wait_for_detector_t;

/** Instance of the detector timing object
 */
static lspg_wait_for_detector_t lspg_wait_for_detector;

/** initialize the detector timing object
 */
void lspg_wait_for_detector_init() {
  lspg_wait_for_detector.new_value_ready = 0;
  pthread_mutex_init( &(lspg_wait_for_detector.mutex), NULL);
  pthread_cond_init(  &(lspg_wait_for_detector.cond), NULL);
}

/** Callback for the wait for detector query
 */
void lspg_wait_for_detector_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  pthread_mutex_lock( &(lspg_wait_for_detector.mutex));
  lspg_wait_for_detector.new_value_ready = 1;
  pthread_cond_signal(  &(lspg_wait_for_detector.cond));
  pthread_mutex_unlock( &(lspg_wait_for_detector.mutex));
}

/** initiate the wait for detector query
 */
void lspg_wait_for_detector_call() {
  pthread_mutex_lock( &(lspg_wait_for_detector.mutex));
  lspg_wait_for_detector.new_value_ready = 0;
  pthread_mutex_unlock( &(lspg_wait_for_detector.mutex));
  
  lspg_query_push( lspg_wait_for_detector_cb, "SELECT px.lock_detector_test_block()");
}

/** Pause the calling thread until the detector is ready
 *  Called by the MD2 thread.
 */
void lspg_wait_for_detector_wait() {
  pthread_mutex_lock( &(lspg_wait_for_detector.mutex));
  while( lspg_wait_for_detector.new_value_ready == 0)
    pthread_cond_wait( &(lspg_wait_for_detector.cond), &(lspg_wait_for_detector.mutex));
}

/** Done waiting for the detector
 */
void lspg_wait_for_detector_done() {
  pthread_mutex_unlock( &(lspg_wait_for_detector.mutex));
}

/** Combined call to wait for the detector.
 * 
 */
void lspg_wait_for_detector_all() {
  lspg_wait_for_detector_call();
  lspg_wait_for_detector_wait();
  lspg_wait_for_detector_done();
}


/** Object used to impliment locking the diffractometer
 *  Critical to exposure timing
 */
typedef struct lspg_lock_diffractometer_struct {
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  int new_value_ready;
} lspg_lock_diffractometer_t;
static lspg_lock_diffractometer_t lspg_lock_diffractometer;

/** initialize the diffractometer locking object
 */
void lspg_lock_diffractometer_init() {
  lspg_lock_diffractometer.new_value_ready = 0;
  pthread_mutex_init( &(lspg_lock_diffractometer.mutex), NULL);
  pthread_cond_init(  &(lspg_lock_diffractometer.cond), NULL);
}

/** Callback routine for a lock diffractometer query
 */
void lspg_lock_diffractometer_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  pthread_mutex_lock( &(lspg_lock_diffractometer.mutex));
  lspg_lock_diffractometer.new_value_ready = 1;
  pthread_cond_signal( &(lspg_lock_diffractometer.cond));
  pthread_mutex_unlock( &(lspg_lock_diffractometer.mutex));
}

/** Request that the database grab the diffractometer lock
 */
void lspg_lock_diffractometer_call() {
  pthread_mutex_lock( &(lspg_lock_diffractometer.mutex));
  lspg_lock_diffractometer.new_value_ready = 0;
  pthread_mutex_unlock( &(lspg_lock_diffractometer.mutex));

  lspg_query_push( lspg_lock_diffractometer_cb, "SELECT px.lock_diffractomter()");
}

/** Wait for the diffractometer lock
 */
void lspg_lock_diffractometer_wait() {
  pthread_mutex_lock( &(lspg_lock_diffractometer.mutex));
  while( lspg_lock_diffractometer.new_value_ready == 0)
    pthread_cond_wait( &(lspg_lock_diffractometer.cond), &(lspg_lock_diffractometer.mutex));
}

/** Finish up the lock diffractometer call
 */
void lspg_lock_diffractometer_done() {
  pthread_mutex_unlock( &(lspg_lock_diffractometer.mutex));
}

/** Convience function that combines lock diffractometer calls
 */
void lspg_lock_diffractometer_all() {
  lspg_lock_diffractometer_call();
  lspg_lock_diffractometer_wait();
  lspg_lock_diffractometer_all();
}

/** lock detector object
 *  Implements detector lock for exposure control
 */
typedef struct lspg_lock_detector_struct {
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  int new_value_ready;
} lspg_lock_detector_t;
static lspg_lock_detector_t lspg_lock_detector;

/** Initialize detector lock object
 */
void lspg_lock_detector_init() {
  lspg_lock_detector.new_value_ready = 0;
  pthread_mutex_init( &(lspg_lock_detector.mutex), NULL);
  pthread_cond_init(  &(lspg_lock_detector.cond),  NULL);
}

/** Callback for when the detector lock has be grabbed
 */
void lspg_lock_detector_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  pthread_mutex_lock( &(lspg_lock_detector.mutex));
  lspg_lock_detector.new_value_ready = 1;
  pthread_cond_signal( &(lspg_lock_detector.cond));
  pthread_mutex_unlock( &(lspg_lock_detector.mutex));
}

/** Request (demand) a detector lock
 */
void lspg_lock_detector_call() {
  pthread_mutex_lock( &(lspg_lock_detector.mutex));
  lspg_lock_detector.new_value_ready = 0;
  pthread_mutex_unlock( &(lspg_lock_detector.mutex));

  lspg_query_push( lspg_lock_detector_cb, "SELECT px.lock_detector()");
}

/** Wait for the detector lock
 */
void lspg_lock_detector_wait() {
  pthread_mutex_lock( &(lspg_lock_detector.mutex));
  while( lspg_lock_detector.new_value_ready == 0)
    pthread_cond_wait( &(lspg_lock_detector.cond), &(lspg_lock_detector.mutex));
}

/** Finish waiting.
 */
void lspg_lock_detector_done() {
  pthread_mutex_unlock( &(lspg_lock_detector.mutex));
}

/** Detector lock convinence function
 */
void lspg_lock_detector_all() {
  lspg_lock_detector_call();
  lspg_lock_detector_wait();
  lspg_lock_detector_done();
}

/** Data collection running object
 */
typedef struct lspg_seq_run_prep_struct {
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  int new_value_ready;
} lspg_seq_run_prep_t;
static lspg_seq_run_prep_t lspg_seq_run_prep;

/** Initialize the data collection object
 */
void lspg_seq_run_prep_init() {
  lspg_seq_run_prep.new_value_ready = 0;
  pthread_mutex_init( &(lspg_seq_run_prep.mutex), NULL);
  pthread_cond_init(  &(lspg_seq_run_prep.cond),  NULL);
}

/** Callback for the seq_run_prep query
 */
void lspg_seq_run_prep_cb(
			  lspg_query_queue_t *qqp,	/**< [in] The query item that generated this callback	*/
			  PGresult *pgr			/**< [in] The result of the query			*/
			  ) {
  pthread_mutex_lock( &(lspg_seq_run_prep.mutex));
  lspg_seq_run_prep.new_value_ready = 1;
  pthread_cond_signal( &(lspg_seq_run_prep.cond));
  pthread_mutex_unlock( &(lspg_seq_run_prep.mutex));
}

/** queue up the seq_run_prep query
 */
void lspg_seq_run_prep_call(
			    long long skey,		/**< [in] px.shots key for this image			*/
			    double kappa,		/**< [in] current kappa postion				*/
			    double phi,			/**< [in] current phi postition				*/
			    double cx,			/**< [in] current center table x			*/
			    double cy,			/**< [in] current center table y			*/
			    double ax,			/**< [in] current alignment table x			*/
			    double ay,			/**< [in] current alignment table y			*/
			    double az			/**< [in] current alignment table z			*/
			    ) {
  pthread_mutex_lock( &(lspg_seq_run_prep.mutex));
  lspg_seq_run_prep.new_value_ready = 0;
  pthread_mutex_unlock( &(lspg_seq_run_prep.mutex));

  lspg_query_push( lspg_seq_run_prep_cb, "SELECT px.seq_run_prep( %lld, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f)",
		   skey, kappa, phi, cx, cy, ax, ay, az);
}

/** Wait for seq run prep query to return
 */
void lspg_seq_run_prep_wait() {
  pthread_mutex_lock( &(lspg_seq_run_prep.mutex));
  while( lspg_seq_run_prep.new_value_ready == 0)
    pthread_cond_wait( &(lspg_seq_run_prep.cond), &(lspg_seq_run_prep.mutex));
}

/** Indicate we are done waiting
 */
void lspg_seq_run_prep_done() {
  pthread_mutex_unlock( &(lspg_seq_run_prep.mutex));
}

/** Convinence function to call seq run prep
 */
void lspg_seq_run_prep_all(
			    long long skey,		/**< [in] px.shots key for this image			*/
			    double kappa,		/**< [in] current kappa postion				*/
			    double phi,			/**< [in] current phi postition				*/
			    double cx,			/**< [in] current center table x			*/
			    double cy,			/**< [in] current center table y			*/
			    double ax,			/**< [in] current alignment table x			*/
			    double ay,			/**< [in] current alignment table y			*/
			    double az			/**< [in] current alignment table z			*/
			   ) {
  lspg_seq_run_prep_call( skey, kappa, phi, cx, cy, ax, ay, az);
  lspg_seq_run_prep_wait();
  lspg_seq_run_prep_done();
}

/** Retrieve the data to center the crystal
 */
void lspg_getcenter_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  static int
    zoom_c, dcx_c, dcy_c, dax_c, day_c, daz_c;

  pthread_mutex_lock( &(lspg_getcenter.mutex));
  
  lspg_getcenter.no_rows_returned = PQntuples( pgr) <= 0;
  if( lspg_getcenter.no_rows_returned) {
    //
    // No particular reason this path should ever be taken
    // but if we don't get rows then we had better not move anything.
    //
    lspg_getcenter.new_value_ready = 1;
    pthread_cond_signal( &(lspg_getcenter.cond));
    pthread_mutex_unlock( &(lspg_getcenter.mutex));
    return;
  }

  zoom_c = PQfnumber( pgr, "zoom");
  dcx_c  = PQfnumber( pgr, "dcx");
  dcy_c  = PQfnumber( pgr, "dcy");
  dax_c  = PQfnumber( pgr, "dax");
  day_c  = PQfnumber( pgr, "day");
  daz_c  = PQfnumber( pgr, "daz");

  lspg_getcenter.zoom_isnull = PQgetisnull( pgr, 0, zoom_c);
  if( lspg_getcenter.zoom_isnull == 0)
    lspg_getcenter.zoom = atoi( PQgetvalue( pgr, 0, zoom_c));

  lspg_getcenter.dcx_isnull = PQgetisnull( pgr, 0, dcx_c);
  if( lspg_getcenter.dcx_isnull == 0)
    lspg_getcenter.dcx = atof( PQgetvalue( pgr, 0, dcx_c));

  lspg_getcenter.dcy_isnull = PQgetisnull( pgr, 0, dcy_c);
  if( lspg_getcenter.dcy_isnull == 0)
    lspg_getcenter.dcy = atof( PQgetvalue( pgr, 0, dcy_c));

  lspg_getcenter.dax_isnull = PQgetisnull( pgr, 0, dax_c);
  if( lspg_getcenter.dax_isnull == 0)
    lspg_getcenter.dax = atof( PQgetvalue( pgr, 0, dax_c));

  lspg_getcenter.day_isnull = PQgetisnull( pgr, 0, day_c);
  if( lspg_getcenter.day_isnull == 0)
    lspg_getcenter.day = atof( PQgetvalue( pgr, 0, day_c));

  lspg_getcenter.daz_isnull = PQgetisnull( pgr, 0, daz_c);
  if( lspg_getcenter.daz_isnull == 0)
    lspg_getcenter.daz = atof( PQgetvalue( pgr, 0, daz_c));

  lspg_getcenter.new_value_ready = 1;

  pthread_cond_signal( &(lspg_getcenter.cond));
  pthread_mutex_unlock( &(lspg_getcenter.mutex));
}

/** Initialize getcenter object
 */
void lspg_getcenter_init() {
  memset( &lspg_getcenter, 0, sizeof( lspg_getcenter));
  pthread_mutex_init( &(lspg_getcenter.mutex), NULL);
  pthread_cond_init( &(lspg_getcenter.cond), NULL);
}

/** Request a getcenter query
 */
void lspg_getcenter_call() {
  pthread_mutex_lock( &lspg_getcenter.mutex);
  lspg_getcenter.new_value_ready = 0;
  pthread_mutex_unlock( &lspg_getcenter.mutex);

  lspg_query_push( lspg_getcenter_cb, "SELECT * FROM px.getcenter2()");
}

/** Wait for a getcenter query to return
 */
void lspg_getcenter_wait() {
  pthread_mutex_lock( &(lspg_getcenter.mutex));
  while( lspg_getcenter.new_value_ready == 0)
    pthread_cond_wait( &(lspg_getcenter.cond), &(lspg_getcenter.mutex));
}

/** Done with getcenter query
 */
void lspg_getcenter_done() {
  pthread_mutex_unlock( &(lspg_getcenter.mutex));
}

/** Convenience function to complete synchronous getcenter query
 */
void lspg_getcenter_all() {
  lspg_getcenter_call();
  lspg_getcenter_wait();
  lspg_getcenter_done();
}


/** Queue the next MD2 instruction
 */
void lspg_nextaction_cb(
			lspg_query_queue_t *qqp,	/**< [in] The query that generated this result		*/
			PGresult *pgr			/**< [in] The result					*/
			) {
  char *action;

  if( PQntuples( pgr) <= 0)
    return;		// Note: nextaction should always return at least "noAction", so this branch should never be taken

  action = PQgetvalue( pgr, 0, 0);	// next action only returns one row

  if( strcmp( action, "noAction") == 0)
    return;
  
  if( pthread_mutex_trylock( &md2cmds_mutex) == 0) {
    strncpy( md2cmds_cmd, action, MD2CMDS_CMD_LENGTH-1);
    md2cmds_cmd[MD2CMDS_CMD_LENGTH-1] = 0;
    pthread_cond_signal( &md2cmds_cond);
    pthread_mutex_unlock( &md2cmds_mutex);
  } else {
    lslogging_log_message( "MD2 command '%s' ignored.  Already running '%s'", action, md2cmds_cmd);
  }
}

/** retrieve kv pairs with new values
 */
void lspg_kvs_cb(
		 lspg_query_queue_t *qqp,		/**< [in] Our query					*/
		 PGresult *pgr				/**< [in] Our result					*/
		 ) {
  int i;

  lslogging_log_message( "lspg_kvs_cb: %d tuples", PQntuples(pgr));

  // Even i is key  (the name)
  // Odd  i is value
  //
  for( i=0; i<PQntuples(pgr)/2; i++) {
    lskvs_set( PQgetvalue( pgr, 2*i, 0), PQgetvalue( pgr, 2*i+1, 0));
  }
}
/** Send strings directly to PMAC queue
 */
void lspg_cmd_cb(
		 lspg_query_queue_t *qqp,		/**< [in] Our query					*/
		 PGresult *pgr				/**< [in] Our result					*/
		 ) {
  //
  // Call back funciton assumes query results in zero or more commands to send to the PMAC
  //
  int i;
  char *sp;
  
  for( i=0; i<PQntuples( pgr); i++) {
    sp = PQgetvalue( pgr, i, 0);
    if( sp != NULL && *sp != 0) {
      lspmac_SockSendline( sp);
      //
      // Keep asking for more until
      // there are no commands left
      // 
      // This should solve a potential problem where
      // more than one command is put on the queue for a given notify.
      //
      lspg_query_push( lspg_cmd_cb, "select pmac.md2_queue_next()");
    }
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

    lslogging_log_message( "flush failed: %s", PQerrorMessage( q));

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
    lslogging_log_message( "Popped empty query string.  Probably bad things are going on.");

    lspg_query_reply_next();
    ls_pg_state = LS_PG_STATE_IDLE;
  } else {
    err = PQsendQuery( q, qqp->qs);
    if( err == 0) {
      lslogging_log_message( "query failed: %s\n", PQerrorMessage( q));

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

/** Receive a result of a query
 */
void lspg_receive() {
  PGresult *pgr;
  lspg_query_queue_t *qqp;
  int err;

  err = PQconsumeInput( q);
  if( err != 1) {
    lslogging_log_message( "consume input failed: %s", PQerrorMessage( q));
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
	  lslogging_log_message( "Error from query '%s':\n%s", qqp->qs, emess);
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

/** Service a signal
 *  Signals here are treated as file descriptors
 *  and fits into our poll scheme
 */
void lspg_sig_service(
		      struct pollfd *evt		/**< [in] The pollfd object that triggered this call	*/
		      ) {
  struct signalfd_siginfo fdsi;

  //
  // Really, we don't care about the signal,
  // it's just used to drop out of the poll
  // function when there is something for us
  // to do that didn't invovle something coming
  // from our postgresql server.
  //
  // This is accompished by the query_push function
  // to notify us that a new query is ready.
  //

  read( evt->fd, &fdsi, sizeof( struct signalfd_siginfo));

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
	lslogging_log_message( "consume input failed: %s", PQerrorMessage( q));
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
	
	if( strstr( pgn->relname, "_pmac") != NULL) {
	  lspg_query_push( lspg_cmd_cb, "SELECT pmac.md2_queue_next()");
	} else if (strstr( pgn->relname, "_diff") != NULL) {
	  lspg_query_push( lspg_nextaction_cb, "SELECT action FROM px.nextaction()");
	} else if (strstr( pgn->relname, "_kvs") != NULL) {
	  lspg_query_push( lspg_kvs_cb, "SELECT pmac.getkvs()");
	}
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

PQnoticeProcessor lspg_notice_processor( void *arg, const char *msg) {
  lslogging_log_message( "lspg: %s", msg);
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
      lslogging_log_message( "Out of memory (lspg_pg_connect)");
      exit( -1);
    }

    err = PQstatus( q);
    if( err == CONNECTION_BAD) {
      lslogging_log_message( "Trouble connecting to database");

      gettimeofday( &lspg_time_sent, NULL);
      return;
    }
    err = PQsetnonblocking( q, 1);
    if( err != 0) {
      lslogging_log_message( "Odd, could not set database connection to nonblocking");
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
      lspg_query_push( lspg_init_motors_cb, "select * from pmac.md2_getmotors()");
      lspg_query_push( NULL, "select pmac.md2_init()");
      lspg_query_push( lspg_zoom_lut_cb, "SELECT * FROM pmac.md2_zoom_lut()");
      lspg_query_push( lspg_flight_lut_cb, "SELECT * FROM pmac.md2_flight_lut()");
      lspg_query_push( lspg_blight_lut_cb, "SELECT * FROM pmac.md2_blight_lut()");
      lspg_query_push( lspg_scint_lut_cb,      "SELECT * FROM pmac.md2_scint_lut()");

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
      lspg_query_push( lspg_init_motors_cb, "select * from pmac.md2_getmotors()");
      lspg_query_push( NULL, "select pmac.md2_init()");
      ls_pg_state = LS_PG_STATE_IDLE;
    }
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

/** The main loop for the lspg thread
 */
void *lspg_worker(
		  void *dummy		/**< [in] Required by pthreads but unused		*/
		  ) {
  static struct pollfd fda[2];	// 0=signal handler, 1=pg socket
  static int nfda = 0;
  static sigset_t our_sigset;
  int sigfd;

  sigemptyset( &our_sigset);
  sigaddset( &our_sigset, SIGUSR1);


  //
  // block ordinary signal mechanism
  //
  sigprocmask(SIG_BLOCK, &our_sigset, NULL);

    
  fda[0].fd = signalfd( -1, &our_sigset, SFD_NONBLOCK);
  if( fda[0].fd == -1) {
    char *es;

    es = strerror( errno);
    lslogging_log_message( "Signalfd trouble: %s", es);
  }
  fda[0].events = POLLIN;

  //
  //  make sure file descriptor is not legal until it's been conneceted
  //
  lspgfd.fd   = -1;


  while( 1) {
    int pollrtn;
    int poll_timeout_ms;

   lspg_next_state();

    if( lspgfd.fd == -1) {
      //
      // Here a connection to the database is not established.
      // Periodicaly try again.  Should possibly arrange to reconnect
      // to signalfd but that's unlikely to be nessesary.
      //
      nfda = 1;
      poll_timeout_ms = 10000;
      fda[1].revents = 0;
    } else {
      //
      // Arrange to peacfully do nothing until either the pg server sends us something
      // or someone pushs something onto our queue
      //
      nfda = 2;
      fda[1].fd      = lspgfd.fd;
      fda[1].events  = lspgfd.events;
      fda[1].revents = 0;
      poll_timeout_ms = -1;
    }

    pollrtn = poll( fda, nfda, poll_timeout_ms);

    if( pollrtn && fda[0].revents) {
      lspg_sig_service( &(fda[0]));
      pollrtn--;
    } 
    if( pollrtn && fda[1].revents) {
      lspg_pg_service( &(fda[1]));
      pollrtn--;
    } 




  }
}


/** Initiallize the lspg module
 */
void lspg_init() {
  pthread_mutex_init( &lspg_queue_mutex, NULL);
  pthread_cond_init( &lspg_queue_cond, NULL);
  lspg_nextshot_init();
  lspg_getcenter_init();
  lspg_wait_for_detector_init();
  lspg_lock_diffractometer_init();
  lspg_lock_detector_init();
}

/** Start 'er runnin'
 */
void lspg_run() {
  pthread_create( &lspg_thread, NULL, lspg_worker, NULL);
}
