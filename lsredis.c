#include "pgpmac.h"
/*! \file lsredis.c
 *  \brief Support redis hash synchronization
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 *
 * \details
 *
 * Redis support for redis in pgpmac.
 *
 *  Values in redis are assumed to be hashs with at list one field "VALUE".
 *  At startup the initialization routine is passed a regular expression to select which keys
 *  we'd like to duplicate locally as a lsredis_obj_t.  It is assumed that the following construct
 *  in redis is used to change a value:
 *
 <pre>
    MULTI
    HSET key VALUE value
    PUBLISH publisher key
    EXEC
</pre>
 *
 * Where "publisher" is a unique name in the following format:
<pre>
         MD2-*
   or    UI-*
   or    REDIS_KV_CONNECTOR
   or    mk_pgpmac_redis
</pre>
 * (this last value is used to support the now deprecated px.kvs table in the LS-CAT postgresql server).
 * We assume that all publishers that we are listening to ONLY publish key names that have changed.
 *
 * When someone else changes a value we invalidate our internal copy and issue a "HGET key VALUE" command.  Other threads
 * that request the value of our lsredis_obj_t will pause until the new value has been received and processed.
 *
 * When a value changes locally this module changes it in redis as shown above.  At this point we refuse
 * other publishers attempt to change the value until we've seen all of our PUBLISH messages.  That is, we ignore
 * changes that in redis happened before our change.
 *
 * You'll need an lsredis_obj_t to do anything with redis in the pgpmac project:
 * <pre>
 * lsredis_obj_t *lsredis_get_obj( char *fmt, ...)  where fmt is a printf style formatting string to interpret the rest of the arguments (if any)
 *                                                  During initialization a "head" string is passed that is prepended to form the redis key.
 *                                                  For example, "omega.position" might refer to the key "stns.2.omega.position" at LS-CAT.
 * </pre>
 *
 * To set a redis value use
 * <pre>
 *  void lsredis_setstr( lsredis_obj_t *p, char *fmt, ...)  where fmt is a printf style formatting string to interpret the rest of the arguments (if any)
 * </pre>
 *
 * When a new value is seen we immediately parse it and make it available through the following functions:
 * <pre>
 *
 *   char    *lsredis_getstr( lsredis_obj_t *p)            Returns a copy of the VALUE field.  Use "free" on the retured value when done using it.
 *
 *   double   lsredis_getd( lsredis_obj_t *p)              Returns a double.  If the value was not a number it returns 0.
 *
 *   long int lsredis_getl( lsredis_obj_t *p)              Returns a long int.  If the value was not a number it returns 0.
 *
 *   char   **lsredis_get_string_array( lsredis_obj_t *p)  Returns an array of string pointers.  Value is assumed formated as a postgresql array, ie, {here,"I am","for example"}.
 *                                                or NULL if the value could not be parsed
 *
 *   int      lsredis_getb( lsredis_obj_t *p)              Returns 1, 0, or -1 based on the fist character of the string. 1 for T,t,Y,y, or 1, 0 for F,f,N,n or 0, -1 for anything else.
 *
 *   char     lsredis_getc( lsredis_obj_t *p)              Returns the first character of VALUE
 *
 * </pre>
 */

static pthread_t lsredis_thread;

pthread_mutex_t lsredis_mutex;
pthread_cond_t  lsredis_cond;
int lsredis_running = 0;
static pthread_mutexattr_t mutex_initializer;


static lsredis_obj_t *lsredis_objs = NULL;
static struct hsearch_data lsredis_htab;

static redisAsyncContext *subac;
static redisAsyncContext *roac;
static redisAsyncContext *wrac;

static char *lsredis_publisher = NULL;
static regex_t lsredis_key_select_regex;
static char *lsredis_head = NULL;
static pthread_mutex_t lsredis_config_mutex;
static pthread_cond_t  lsredis_config_cond;

static struct pollfd subfd;
static struct pollfd rofd;
static struct pollfd wrfd;

typedef struct lsredis_preset_list_struct {
  struct lsredis_preset_list_struct *next;	// next in the list or null if none
  char *key;					// our key (motor name concatenated with the preset name)
  int index;					// our index in the motor's preset array
  lsredis_obj_t *name;				// object containing the name
  lsredis_obj_t *position;			// object containing the position
} lsredis_preset_list_t;

static lsredis_preset_list_t *lsredis_preset_list = NULL;	// our list of presets
static struct hsearch_data lsredis_preset_ht;			// hash table to find list items
static int lsredis_preset_n = 0;				// number of entries in the hash table
static int lsredis_preset_max_n = 1024;				// size of the hash table
static pthread_mutex_t lsredis_preset_list_mutex;

/** Log the reply
 */
void lsredis_debugCB( redisAsyncContext *ac, void *reply, void *privdata) {
  static int indentlevel = 0;
  redisReply *r;
  int i;

  r = (redisReply *)reply;

  if( r == NULL) {
    lslogging_log_message( "Null reply.  Odd");
    return;
  }

  switch( r->type) {
  case REDIS_REPLY_STATUS:
    lslogging_log_message( "%*sSTATUS: %s", indentlevel*4,"", r->str);
    break;

  case REDIS_REPLY_ERROR:
    lslogging_log_message( "%*sERROR: %s", indentlevel*4, "", r->str);
    break;

  case REDIS_REPLY_INTEGER:
    lslogging_log_message( "%*sInteger: %lld", indentlevel*4, "", r->integer);
    break;

  case REDIS_REPLY_NIL:
    lslogging_log_message( "%*s(nil)", indentlevel*4, "");
    break;

  case REDIS_REPLY_STRING:
    lslogging_log_message( "%*sSTRING: %s", indentlevel*4, "", r->str);
    break;

  case REDIS_REPLY_ARRAY:
    lslogging_log_message( "%*sARRAY of %d elements", indentlevel*4, "", (int)r->elements);
    indentlevel++;
    for( i=0; i<(int)r->elements; i++)
      lsredis_debugCB( ac, r->element[i], NULL);
    indentlevel--;
    break;

  default:
    lslogging_log_message( "%*sUnknown type %d", indentlevel*4,"", r->type);

  }
}

/**
 * notify callback when a variable is set
 */
void lsredis_set_onSet( lsredis_obj_t *p, void (*cb)()) {
  if( p == NULL)
    return;

  pthread_mutex_lock( &p->mutex);
  p->onSet = cb;
  pthread_mutex_unlock( &p->mutex);
}

/** set_value and setstr helper funciton
 *  p->mutex must be locked before calling
 */
void _lsredis_set_value( lsredis_obj_t *p, char *v) {

  if( strlen(v) >= (unsigned int) p->value_length) {
    if( p->value != NULL)
      free( p->value);
    p->value_length = strlen(v) + 256;
    p->value = calloc( p->value_length, sizeof( char));
    if( p->value == NULL) {
      lslogging_log_message( "_lsredis_set_value: out of memory");
      exit( -1);
    }
  }
  strncpy( p->value, v, p->value_length - 1);
  p->value[p->value_length-1] = 0;
  p->dvalue = strtod( p->value, NULL);
  p->lvalue = p->dvalue;

  if( p->avalue != NULL) {
    int i;
    for( i=0; (p->avalue)[i] != NULL; i++)
      free( (p->avalue)[i]);
    free( p->avalue);
    p->avalue = NULL;
  }

  p->avalue = lspg_array2ptrs( p->value);

  switch( *(p->value)) {
      case 'T':
      case 't':
      case 'Y':
      case 'y':
      case '1':
	p->bvalue = 1;
      break;

      case 'F':
      case 'f':
      case 'N':
      case 'n':
      case '0':
	p->bvalue = 0;
      break;

      default:
	p->bvalue = -1;		// nil is -1 here in our world
    }

  p->cvalue = *(p->value);

  p->valid = 1;				//!< We can consider this value valid
  p->creating = 0;			//!< At this point the key is considered created

  if( p->onSet)
    p->onSet();
}

/** Set the value of a redis object and make it valid.  Called by mgetCB to set the value as it is in redis
 *  Maybe TODO: we've arbitrarily set the maximum size of a value here.
 *  Although I cannot imagine needed bigger values it would not be a big deal to
 *  enable it.
 */
void lsredis_set_value( lsredis_obj_t *p, char *fmt, ...) {
  va_list arg_ptr;
  char v[512];
  
  va_start( arg_ptr, fmt);
  vsnprintf( v, sizeof(v)-1, fmt, arg_ptr);
  va_end( arg_ptr);

  v[sizeof(v)-1] = 0;

  pthread_mutex_lock( &p->mutex);

  _lsredis_set_value( p, v);

  pthread_cond_signal( &p->cond);
  pthread_mutex_unlock( &p->mutex);
}

int lsredis_cmpstr( lsredis_obj_t *p, char *s) {
  int rtn;
  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);
  
  rtn = strcmp( p->value, s);
  pthread_mutex_unlock( &p->mutex);
  return rtn;
}

int lsredis_cmpnstr( lsredis_obj_t *p, char *s, int n) {
  int rtn;
  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);
  
  rtn = strncmp( p->value, s, n);
  pthread_mutex_unlock( &p->mutex);
  return rtn;
}

int lsredis_regexec( const regex_t *preg, lsredis_obj_t *p, size_t nmatch, regmatch_t *pmatch, int eflags) {
  int rtn = 0;
  if (preg->re_nsub <= 0) {
    // Treat the regex as a "filter-in" regex filter.
    // If it doesn't exist yet, treat it as a "no-match"
    return REG_NOMATCH;
  }

  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0) 
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = regexec( preg, p->value, nmatch, pmatch, eflags);

  pthread_mutex_unlock( &p->mutex);

  return rtn;
}

/** return a copy of the key's string value
 *  be sure to free the result
 */
char *lsredis_getstr( lsredis_obj_t *p) {
  char *rtn;

  //
  // Have to use strdup since we cannot guarantee that p->value won't be freed while the caller is still using it
  //
  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = strdup(p->value);
  pthread_mutex_unlock( &p->mutex);
  return rtn;
}


/** Set the value and update redis.
 *  Note that lsredis_set_value sets the value based on redis
 *  while here we set redis based on the value
 *  Arbitray maximum string length set here.  TODO: Probably this limit
 *  should be removed at some point.
 *
 * redisAsyncCommandArgv used instead of redisAsyncCommand 'cause it's easier (and possible)
 * to deal with strings that would otherwise cause hiredis to emit a bad command, like those containing spaces.
 */
void lsredis_setstr( lsredis_obj_t *p, char *fmt, ...) {
  va_list arg_ptr;
  char v[512];
  char *argv[4];

  va_start( arg_ptr, fmt);
  vsnprintf( v, sizeof(v)-1, fmt, arg_ptr);
  v[sizeof(v)-1] = 0;
  va_end( arg_ptr);
  
  pthread_mutex_lock( &p->mutex);

  //
  // Don't send an update if a good value has not changed
  //
  if( p->creating == 0 && p->valid && strcmp( v, p->value) == 0) {
    // nothing to do
    pthread_mutex_unlock( &p->mutex);
    return;
  }

  p->wait_for_me++;			//!< up the count of times we need to see ourselves published before we start listening to others again
  pthread_mutex_unlock( &p->mutex);	//!< Unlock to prevent deadlock in case the service routine needs to set our value


  argv[0] = "HSET";
  argv[1] = p->key;
  argv[2] = "VALUE";
  argv[3] = v;		//!< redisAsyncCommandArgv shouldn't need to access this after it's made up its packet (before it returns) so we should be OK with this location disappearing soon.


  pthread_mutex_lock( &lsredis_mutex);
  while( lsredis_running == 0)
    pthread_cond_wait( &lsredis_cond, &lsredis_mutex);

  redisAsyncCommand( wrac, NULL, NULL, "MULTI");
  redisAsyncCommandArgv( wrac, NULL, NULL, 4, (const char **)argv, NULL);

  redisAsyncCommand( wrac, NULL, NULL, "PUBLISH %s %s", lsredis_publisher, p->key);
  redisAsyncCommand( wrac, NULL, NULL, "EXEC");
  pthread_mutex_unlock( &lsredis_mutex);

  // Assume redis will take exactly the value we sent it
  //
  pthread_mutex_lock( &p->mutex);
  _lsredis_set_value( p, v);
  p->creating = 0;			//!< At this point the key is considered created
  pthread_cond_signal( &p->cond);
  pthread_mutex_unlock( &p->mutex);
}


double lsredis_get_or_set_d( lsredis_obj_t *p, double val, int prec) {
  long int rtn;
  int err;
  struct timespec timeout;

  clock_gettime( CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 2;

  pthread_mutex_lock( &p->mutex);
  err = 0;
  while( err == 0 && p->valid == 0)
    err = pthread_cond_timedwait( &p->cond, &p->mutex, &timeout);

  if( err == ETIMEDOUT) {
    rtn = val;
    pthread_mutex_unlock( &p->mutex);
    lsredis_setstr( p, "%.*f", prec, val);
  } else {
    rtn = p->lvalue;
    pthread_mutex_unlock( &p->mutex);
  }
  
  return rtn;
}  

double lsredis_getd( lsredis_obj_t *p) {
  double rtn;

  pthread_mutex_lock( &p->mutex);
  if( p->creating != 0) {
    pthread_mutex_unlock( &p->mutex);
    lsredis_setstr( p, "");
    pthread_mutex_lock( &p->mutex);
  }

  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->dvalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}

long int lsredis_getl( lsredis_obj_t *p) {
  long int rtn;

  pthread_mutex_lock( &p->mutex);
  if( p->creating != 0) {
    pthread_mutex_unlock( &p->mutex);
    lsredis_setstr( p, "");
    pthread_mutex_lock( &p->mutex);
  }

  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->lvalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}  

long int lsredis_get_or_set_l( lsredis_obj_t *p, long int val) {
  long int rtn;
  int err;
  struct timespec timeout;

  clock_gettime( CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 2;

  pthread_mutex_lock( &p->mutex);
  err = 0;
  while( err == 0 && p->valid == 0)
    err = pthread_cond_timedwait( &p->cond, &p->mutex, &timeout);

  if( err == ETIMEDOUT) {
    lslogging_log_message( "lsredis_get_or_set_l: using default value of %ld for redis variable %s", val, p->key);
    rtn = val;
    pthread_mutex_unlock( &p->mutex);
    lsredis_setstr( p, "%ld", val);
  } else {
    rtn = p->lvalue;
    pthread_mutex_unlock( &p->mutex);
  }
  
  return rtn;
}  

char **lsredis_get_string_array( lsredis_obj_t *p) {
  char **rtn;

  pthread_mutex_lock( &p->mutex);
  if( p->creating != 0) {
    pthread_mutex_unlock( &p->mutex);
    lsredis_setstr( p, "");
    pthread_mutex_lock( &p->mutex);
  }

  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->avalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}

int lsredis_getb( lsredis_obj_t *p) {
  int rtn;

  pthread_mutex_lock( &p->mutex);
  if( p->creating != 0) {
    pthread_mutex_unlock( &p->mutex);
    lsredis_setstr( p, "");
    pthread_mutex_lock( &p->mutex);
  }
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->bvalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}  

char lsredis_getc( lsredis_obj_t *p) {
  int rtn;

  pthread_mutex_lock( &p->mutex);
  if( p->creating != 0) {
    pthread_mutex_unlock( &p->mutex);
    lsredis_setstr( p, "");
    pthread_mutex_lock( &p->mutex);
  }
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->cvalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}  

void lsredis_hgetCB( redisAsyncContext *ac, void *reply, void *privdata) {
  redisReply *r;
  lsredis_obj_t *p;

  r = reply;
  p =  privdata;

  //lslogging_log_message( "hgetCB: %s %s", p == NULL ? "<NULL>" : p->key, r->type == REDIS_REPLY_STRING ? r->str : "Non-string value.  Why?");

  //
  // Apparently this item does not exist
  // Just set it to an empty string so at least other apps will have the same behaviour as us
  // TODO: figure out a better way to deal with missing key/values
  //
  if( p != NULL && r->type == REDIS_REPLY_NIL) {
    //
    // Show that we are creating this new key
    // if the next request is a read we'll create an empty string value and return it
    // if the next request is a write then we'll create that value and return it
    //
    pthread_mutex_lock( &p->mutex);
    p->creating = 1;
    pthread_mutex_unlock( &p->mutex);
    return;
  }

  if( p != NULL && r->type == REDIS_REPLY_STRING && r->str != NULL) {
    pthread_mutex_lock( &p->mutex);

    _lsredis_set_value( p, r->str);

    pthread_cond_signal( &p->cond);
    pthread_mutex_unlock( &p->mutex);
  }
}


/** Maybe add a new object
 *  Used internally for this module
 * Must be called with lsredis_mutex locked
 */
lsredis_obj_t *_lsredis_get_obj( char *key) {
  lsredis_obj_t *p;
  regmatch_t pmatch[2];
  int err;
  ENTRY htab_input, *htab_output;

  // Dispense with obviously bad keys straight away
  // unless p->valid == 0 in which case we call HGET first
  //
  // TODO: review logic: is there ever a time when valid is zero for a preexisting p and HGET has not been called?
  //       If not then we should just return p without checking for validity.
  //
  if( key == NULL || *key == 0 || strchr( key, ' ') != NULL) {
    lslogging_log_message( "_lsredis_get_obj: bad key '%s'", key == NULL ? "<NULL>" : key);
    return NULL;
  }

  // If the key is already there then just return it
  //

  htab_input.key  = key;
  htab_input.data = NULL;
  errno = 0;
  err = hsearch_r( htab_input, FIND, &htab_output, &lsredis_htab);

  if( err == 0)
    p = NULL;
  else
    p = htab_output->data;


  if( p != NULL) {
    return p;
  } else {
    // make a new one.
    p = calloc( 1, sizeof( lsredis_obj_t));
    if( p == NULL) {
      lslogging_log_message( "_lsredis_get_obj: Out of memory");
      exit( -1);
    }

    // The regex is a "filter-in" criteria. If it doesn't exist, we treat it
    // as "no-match" and what we plan to do as "no-go".
    err = lsredis_key_select_regex.re_nsub > 0 ?
      regexec(&lsredis_key_select_regex, key, 2, pmatch, 0) : REG_NOMATCH;
    if (err == 0 && pmatch[1].rm_so != -1) {
      p->events_name = strndup( key+pmatch[1].rm_so, pmatch[1].rm_eo - pmatch[1].rm_so);
    } else {
      p->events_name = strdup( key);
    }
    if( p->events_name == NULL) {
      lslogging_log_message( "_lsredis_get_obj: Out of memory (events_name)");
      exit( -1);
    }

    pthread_mutex_init( &p->mutex, &mutex_initializer);
    pthread_cond_init(  &p->cond, NULL);
    p->value = NULL;
    p->valid = 0;
    // lsevents_send_event( "%s Invalid", p->events_name);
    p->wait_for_me = 0;
    p->key = strdup( key);
    p->hits = 0;
    p->onSet = NULL;

    htab_input.key  = p->key;
    htab_input.data = p;

    errno = 0;
    err = hsearch_r( htab_input, ENTER, &htab_output, &lsredis_htab);
    if( err == 0) {
      lslogging_log_message( "_lsredis_get_obj: hsearch error on enter.  errno=%d: %s", errno, strerror(errno));
    }

    //
    // Shouldn't need the linked list unless we need to rebuild the hash table when, for example, we run out of room.
    // TODO: resize hash table when needed.
    //
    p->next = lsredis_objs;
    lsredis_objs = p;
  }
  //
  // We arrive here with the valid flag lowered.  Go ahead and request the latest value.
  //
  redisAsyncCommand( roac, lsredis_hgetCB, p, "HGET %s VALUE", key);

  return p;
}


lsredis_obj_t *lsredis_get_obj( char *fmt, ...) {
  static const char *id = "lsredis_get_obj";
  lsredis_obj_t *rtn;
  va_list arg_ptr;
  char k[512];
  char *kp;
  int nkp;

  va_start( arg_ptr, fmt);
  vsnprintf( k, sizeof(k)-1, fmt, arg_ptr);
  k[sizeof(k)-1] = 0;
  va_end( arg_ptr);

  if (!lsredis_head) {
    lslogging_log_message("%s: no lsredis_head.  Not good", id);
    return NULL;
  }


  nkp = strlen(k) + strlen( lsredis_head) + 16;		// 16 is overkill. I know. Get over it.
  kp = calloc( nkp, sizeof( char));
  if( kp == NULL) {
    lslogging_log_message( "lsredis_get_obj: Out of memory");
    exit( -1);
  }
  
  snprintf( kp, nkp-1, "%s.%s", lsredis_head, k);
  kp[nkp-1] = 0;

  pthread_mutex_lock( &lsredis_mutex);
  while( lsredis_running == 0)
    pthread_cond_wait( &lsredis_cond, &lsredis_mutex);

  rtn = _lsredis_get_obj( kp);
  pthread_mutex_unlock( &lsredis_mutex);

  free( kp);
  return rtn;
}

/** call back in case a redis server becomes disconnected
 *  TODO: reconnect
 */
void redisDisconnectCB(const redisAsyncContext *ac, int status) {
  if( status != REDIS_OK) {
    lslogging_log_message( "lsredis: Disconnected with status %d", status);
  }
}

/** hook to mange read events
 */
void lsredis_addRead( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;

  if( (pfd->events & POLLIN) == 0) {
    pfd->events |= POLLIN;
    pthread_kill( lsredis_thread, SIGUSR1);
  }
}

/** hook to manage "don't need to read" events
 */
void lsredis_delRead( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;

  if( (pfd->events & POLLIN) != 0) {
    pfd->events &= ~POLLIN;
    pthread_kill( lsredis_thread, SIGUSR1);
  }
}

/** hook to manage write events
 */
void lsredis_addWrite( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;

  if( (pfd->events & POLLOUT) == 0) {
    pfd->events |= POLLOUT;
    pthread_kill( lsredis_thread, SIGUSR1);
  }
}

/** hook to manage "don't need to write anymore" events
 */
void lsredis_delWrite( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;

  if( (pfd->events & POLLOUT) != 0) {
    pfd->events &= ~POLLOUT;
    pthread_kill( lsredis_thread, SIGUSR1);
  }
}

/** hook to clean up
 * TODO: figure out what we are supposed to do here and do it
 */
void lsredis_cleanup( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;

  pfd->fd = -1;

  if( (pfd->events & (POLLOUT | POLLIN)) != 0) {
    pfd->events &= ~(POLLOUT | POLLIN);
    pthread_kill( lsredis_thread, SIGUSR1);
  }
}


/** Use the publication to request the new value
 */
void lsredis_subCB( redisAsyncContext *ac, void *reply, void *privdata) {
  redisReply *r;
  lsredis_obj_t *p;
  char *k;
  char *publisher;
  ENTRY htab_input, *htab_output;
  int err;

  r = (redisReply *)reply;

  // Ignore our psubscribe reply
  //
  if( r->type == REDIS_REPLY_ARRAY && r->elements == 3 && r->element[0]->type == REDIS_REPLY_STRING && strcmp( r->element[0]->str, "psubscribe")==0)
    return;

  // But log other stuff we don't understand
  //
  if( r->type != REDIS_REPLY_ARRAY ||
      r->elements != 4 ||
      r->element[3]->type != REDIS_REPLY_STRING ||
      r->element[2]->type != REDIS_REPLY_STRING) {

    lslogging_log_message( "lsredis_subCB: unexpected reply");
    lsredis_debugCB( ac, reply, privdata);
    return;
  }

  //
  // Ignore obvious junk
  //
  k = r->element[3]->str;

  if( k == NULL || *k == 0)
    return;
  
  //
  // see if we care
  //
  if (lsredis_key_select_regex.re_nsub > 0 &&
      regexec(&lsredis_key_select_regex, k, 0, NULL, 0) == 0) {
    //
    // We should know about this one
    //
    
    htab_input.key  = k;
    htab_input.data = NULL;

    errno = 0;
    err = hsearch_r( htab_input, FIND, &htab_output, &lsredis_htab);
    if( err == 0 && errno == ESRCH)
      p = NULL;
    else
      p = htab_output->data;
      

    if( p == NULL) {
      _lsredis_get_obj( k);
    } else {
      // Look who's talk'n
      publisher = r->element[2]->str;

      pthread_mutex_lock( &p->mutex);
      if( p->wait_for_me) {
	//
	// see if we are done waiting
	//
	if( strcmp( publisher, lsredis_publisher)==0)
	  p->wait_for_me--;

	pthread_mutex_unlock( &p->mutex);
	//
	// Didn't get a new value, either we set it last or we are still waiting for redis to report
	// our publication
	//
	return;
      }

      // Here we know our value is out of date
      //
      p->valid = 0;
      //lsevents_send_event( "%s Invalid", p->events_name);
      pthread_mutex_unlock( &p->mutex);

      //
      // We shouldn't get here if wait_for_me is zero and we are the publisher.
      // If somehow we did (ie we did an hset without incrementing wait_for_me or if we published too many times), it shouldn't hurt to get the value again.
      // 

      redisAsyncCommand( roac, lsredis_hgetCB, p, "HGET %s VALUE", k);
    }
  }
}


void lsredis_maybe_add_key(char *k) {
  // Add the key if it matches our regex filter-in criteria.
  // If there's no regex, don't cache anything.
  if (lsredis_key_select_regex.re_nsub > 0 &&
      regexec(&lsredis_key_select_regex, k, 0, NULL, 0) == 0) {
    if (strstr(k, "state_machine")) {
      lslogging_log_message("lsredis_maybe_add_key adding key %s", k);
    }
    _lsredis_get_obj(k);
  }
}

/** Sift through the keys to find ones we like.  Add them to our list of followed objects
 */
void lsredis_keysCB( redisAsyncContext *ac, void *reply, void *privdata) {
  redisReply *r;
  int i;
  
  r = reply;
  if( r->type != REDIS_REPLY_ARRAY) {
    lslogging_log_message( "lsredis_keysCB: exepected array...");
    lsredis_debugCB( ac, reply, privdata);
    return;
  }
  
  for( i=0; i< (int)r->elements; i++) {
    if( r->element[i]->type != REDIS_REPLY_STRING) {
      lslogging_log_message( "lsredis_keysCB: exepected string...");
      lsredis_debugCB( ac, r->element[i], privdata);
    } else {
      lsredis_maybe_add_key( r->element[i]->str);
    }
  }
}

/** update the presets hash table for the named motor
 */
void lsredis_load_presets( char *motor_name) {
  lsredis_obj_t *p;
  lsredis_preset_list_t *pl;
  int plength;
  char *preset_name;
  int i;
  int key_length;
  ENTRY entry_in, *entry_outp;

  p = lsredis_get_obj( "%s.presets.length", motor_name);
  plength = lsredis_get_or_set_l( p, 0);
  if( plength <= 0)
    return;

  pthread_mutex_lock( &lsredis_preset_list_mutex);

  for( i=0; i<plength; i++) {
    pl = calloc( 1, sizeof( lsredis_preset_list_t));
    pl->name      = lsredis_get_obj( "%s.presets.%d.name",     motor_name, i);
    pl->position  = lsredis_get_obj( "%s.presets.%d.position", motor_name, i);
    pl->index     = i;
    
    preset_name   = lsredis_getstr( pl->name);
    key_length    = strlen( motor_name) + strlen( preset_name) + 1;
    pl->key       = calloc( key_length, 1);

    pl->next            = lsredis_preset_list;
    lsredis_preset_list = pl;

    snprintf( pl->key, key_length, "%s%s", motor_name, preset_name);

    entry_in.key  = pl->key;
    entry_in.data = pl;
    hsearch_r( entry_in, ENTER, &entry_outp, &lsredis_preset_ht);
    if( entry_outp->data != pl) {
      //
      // The key was already there or we couldn't add it
      //
      if( entry_outp->data == NULL)
	lslogging_log_message( "lsredis_load_presets: could not add preset '%s' for motor '%s'", preset_name, motor_name);

      free( pl->key);
      free( pl);
    } else {
      //
      // We've successfully added the new key
      //
      lsredis_preset_n++;
      //
      // Resize the hash table if we are starting to fill it up
      // Generally we prefer a sparse table
      //
      if( lsredis_preset_n >= lsredis_preset_max_n) {
	lslogging_log_message( "lsredis_load_presets: increasing preset hash table size.  max now %d", lsredis_preset_max_n);
	hdestroy_r( &lsredis_preset_ht);
	lsredis_preset_max_n *= 2;
	hcreate_r( 2 * lsredis_preset_max_n, &lsredis_preset_ht);
	for( pl = lsredis_preset_list; pl != NULL; pl = pl->next) {
	  entry_in.key  = pl->key;
	  entry_in.data = pl;
	  hsearch_r( entry_in, ENTER, &entry_outp, &lsredis_preset_ht);
	}
	lslogging_log_message( "lsredis_load_presets: done increasing preset hash table size.", lsredis_preset_max_n);
      }
    }
    free( preset_name);
  }
  pthread_mutex_unlock( &lsredis_preset_list_mutex);
}

/** Get the value of the given preset and return it in dval
 *  Returns 0 on error, non-zero on success;
 */
int lsredis_find_preset( char *motor_name, char *preset_name, double *dval) {
  char s[512];
  int err;
  ENTRY entry_in, *entry_outp;
  lsredis_preset_list_t *pl;

  snprintf( s, sizeof( s)-1, "%s%s", motor_name, preset_name);
  s[sizeof(s)-1] = 0;

  entry_in.key  = s;
  entry_in.data = NULL;
  err = hsearch_r( entry_in, FIND, &entry_outp, &lsredis_preset_ht);

  if( err == 0) {
    // not found (or some other problem that means we don't have an answer
    //
    // Maybe someone added a new preset and we don't know about it yet
    //
    lsredis_load_presets( motor_name);
    err = hsearch_r( entry_in, FIND, &entry_outp, &lsredis_preset_ht);
    if( err == 0) {
      //
      // Guess not.  Give up.  We tried
      //
      lslogging_log_message( "lsredis_find_preset: no preset named '%s' for motor '%s' found", preset_name, motor_name);
      *dval = 0.0;
      return 0;
    }
  }
  pl = entry_outp->data;
  *dval = lsredis_getd( pl->position);
  return 1;
}


/** set the given preset to the given value
 *  create a new preset if we can't find it
 */
void lsredis_set_preset( char *motor_name, char *preset_name, double dval) {
  static const char *id = "lsredis_set_preset";
  char s[512];
  int plength;
  int err;
  double min_pos, max_pos, neutral_pos;
  lspmac_motor_t *mp;
  ENTRY entry_in, *entry_outp;
  lsredis_obj_t *p1, *p2, *presets_length_p;
  lsredis_preset_list_t *pl;
  struct timespec timeout;

  if (preset_name[0] == 0) {
    lslogging_log_message("%s: no preset name given", id);
    return;
  }

  if (lspmac_find_motor_by_name(preset_name) != NULL) {
    lslogging_log_message("%s: Oddly, a preset is being given the same name as a motor.  Probably not what we want: %s", id, preset_name);
    return;
  }

  mp = lspmac_find_motor_by_name( motor_name);
  if( mp) {
    neutral_pos = lsredis_getd( mp->neutral_pos);
    min_pos     = lsredis_getd( mp->min_pos) - neutral_pos;
    max_pos     = lsredis_getd( mp->max_pos) - neutral_pos;
    if( dval < min_pos || dval > max_pos) {
      lslogging_log_message( "Cannot set preset '%s' for motor '%s' to %.3f  min pos: %.3f   max pos: %.3f\n", preset_name, motor_name, dval, min_pos, max_pos);
      lsredis_sendStatusReport( 1, "Point out of range. Try again");
      return;
    }
  }

  snprintf( s, sizeof( s)-1, "%s%s", motor_name, preset_name);
  s[sizeof(s)-1] = 0;

  entry_in.key  = s;
  entry_in.data = NULL;
  pthread_mutex_lock( &lsredis_preset_list_mutex);
  err = hsearch_r( entry_in, FIND, &entry_outp, &lsredis_preset_ht);
  if( err != 0) {
    //
    // Found it.  Things are simple.
    //
    pl = entry_outp->data;
    lsredis_setstr( pl->position, "%.3f", dval);
    pthread_mutex_unlock( &lsredis_preset_list_mutex);
    return;
  }
  //
  // OK, our preset was not found, add it
  //
  presets_length_p = lsredis_get_obj(  "%s.presets.length", motor_name);
  plength = lsredis_get_or_set_l( presets_length_p, 0);
  plength += 1;

  snprintf( s, sizeof( s)-1, "%s.%s.presets.%d.name", lsredis_head, motor_name, plength-1);
  s[sizeof(s)-1] = 0;

  p1 = lsredis_get_obj( "%s.presets.%d.name", motor_name, plength-1);
  pthread_mutex_lock( &p1->mutex);
  err = 0;
  while( err == 0 && p1->valid == 0)
    err = pthread_cond_timedwait( &p1->cond, &p1->mutex, &timeout);

  if( err == ETIMEDOUT) {
    lslogging_log_message( "lsredis_set_preset: timed out waiting for name to become valid");
  }
  pthread_mutex_unlock( &p1->mutex);
  lsredis_setstr( p1, "%s", preset_name);
  
  p2 = lsredis_get_obj( "%s.presets.%d.position", motor_name, plength-1);
  lsredis_setstr( p2, "%.3f", dval);
  
  lsredis_setstr( presets_length_p, "%ld", plength);

  pthread_mutex_unlock( &lsredis_preset_list_mutex);
  lsredis_load_presets( motor_name);
}

/** For the given motor object return the index of the current preset or -1 if we are not at a preset position
 */
int lsredis_find_preset_index_by_position( lspmac_motor_t *mp) {
  lsredis_obj_t *p;
  int plength;
  int i;
  double ur, pos;

  p = lsredis_get_obj( "%s.presets.length", mp->name);
  plength = lsredis_get_or_set_l( p, 0);
  
  if( plength <= 0) {
    return -1;
  }

  ur = lsredis_getd( mp->update_resolution);
  pos = lspmac_getPosition( mp);

  for( i=0; i<plength; i++) {
    p = lsredis_get_obj( "%s.presets.%d.position", mp->name, i);
    if( fabs( pos - lsredis_getd( p)) <= ur) {
      return i;
    }
  }
  return -1;
}

/** For the given motor object return the index of the named prefix or
 ** -1 if the preset is not found
 */
int lsredis_find_preset_index_by_name( lspmac_motor_t *mp, char *searchPresetName) {
  lsredis_obj_t *p;
  char *tmp;
  int plength;
  int i;
  int found;

  p = lsredis_get_obj( "%s.presets.length", mp->name);
  plength = lsredis_get_or_set_l( p, 0);
  if( plength <= 0) {
    return -1;
  }

  found = 0;
  for( i=0; i<plength && !found; i++) {
    p = lsredis_get_obj( "%s.presets.%d.name", mp->name, i);
    tmp = lsredis_getstr( p);
    if( tmp) {
      if( strcmp( tmp, searchPresetName) == 0) {
	found = 1;
      }
      free( tmp);
    }
  }

  if( !found)
    return -1;
  return i;
}

/** send log message to our redis log key
 */
void lsredis_log( char *fmt, ...) {
  static lsredis_obj_t *loghashs[32];
  static int firstTime = 1;
  static int lhi = 0;
  char msg[2048+64];
  va_list arg_ptr;
  
  
  
  if( pthread_mutex_trylock( &lsredis_mutex) == 0) {
    
    
    if( lsredis_head == NULL) {			// Ignore messages that come before redis is initiallized
      pthread_mutex_unlock( &lsredis_mutex);
      return;
    }
    
    if( firstTime) {
      int i;
      for( i=0; i<sizeof(loghashs)/sizeof(loghashs[0]); i++) {
	loghashs[i] = lsredis_get_obj( "log.message.%d", i);
      }
      lhi = 0;
      firstTime=0;
    }
    
    va_start( arg_ptr, fmt);
    vsnprintf( msg, sizeof(msg)-1, fmt, arg_ptr);
    va_end( arg_ptr);
    msg[sizeof(msg)-1]=0;
    
    lsredis_setstr( loghashs[lhi], "%s", msg);
    lhi = (lhi+1) % (sizeof(loghashs)/sizeof(loghashs[0]));
    pthread_mutex_unlock( &lsredis_mutex);
    
  }
}


void lsredis_sendStatusReport( int severity, char *fmt, ...) {
  static lsredis_obj_t *messp  = NULL;
  char msg[256];     // our (almost) processed string
  char emsg[256];   // escaped version of the string.  Using a long string with tons of odd characters will cause truncation.  If you think this is a bug then feel free to fix it.
  int i, j, k;
  int mask, badutf8;
  va_list arg_ptr;

  if( messp == NULL) {
    messp = lsredis_get_obj( "md2.status");
    if( messp == NULL) {
      lslogging_log_message( "Could not find redis object for md2.status");
      return;
    }
  }
  
  va_start( arg_ptr, fmt);
  vsnprintf( msg, sizeof( msg)-1, fmt, arg_ptr);
  va_end( arg_ptr);

  // escape various and sundry things
  //
  // Our goal is to only generate legal JSON even if we are called with garbage: we do not want a wild pointer
  // to kill the user's UI.
  //
  // TODO: find some nice general JSON package and put it in a utilities module
  //
  //
  // From UTF-8 wikipedia page
  //=============+============+=============+==============+==========+==========+==========+==========+==========+==========|
  //    Bits of  |   First    |     Last    |     Bytes in |   Byte 1 | Byte 2   |  Byte 3  |  Byte 4  |  Byte 5  |  Byte 6  |
  //  code point | code point | code point  |  sequence    |          |          |          |          |          |          |
  //=============+============+=============+==============+==========+==========+==========+==========+==========+==========|
  //   7         | U+0000     | U+007F      |      1       | 0xxxxxxx |          |          |          |          |          |
  //  11         | U+0080     | U+07FF      |      2       | 110xxxxx | 10xxxxxx |          |          |          |          |
  //  16         | U+0800     | U+FFFF      |      3       | 1110xxxx | 10xxxxxx | 10xxxxxx |          |          |          |
  //  21         | U+10000    | U+1FFFFF    |      4       | 11110xxx | 10xxxxxx | 10xxxxxx | 10xxxxxx |          |          |
  //  26         | U+200000   | U+3FFFFFF   |      5       | 111110xx | 10xxxxxx | 10xxxxxx | 10xxxxxx | 10xxxxxx |          |
  //  31         | U+4000000  | U+7FFFFFFF  |      6       | 1111110x | 10xxxxxx | 10xxxxxx | 10xxxxxx | 10xxxxxx | 10xxxxxx |
  //=============+============+=============+==============+==========+==========+==========+==========+==========+==========|
  //

  emsg[0] = 0;
  //
  // msg might end with a 6 byte unicode character and we'll still need room for the trailing 0, so emsg will be shorter than msg
  // if someone makes msg wacky long with crap at the end that looks like the start of a unicode sequence.
  //
  for( i=0, j=0; i<sizeof( msg)-1 && j<sizeof(emsg)-7 && msg[i]!=0; i++, j++) {
    //
    // add (only) legal utf-8 characters.
    //
    if( (msg[i] & 0x80) == 0) {
      //
      // OK, ASCII.  Life is good.
      //
      switch( msg[i]) {
      case '"':
      case '\\':
      case '/':
	emsg[j++] = '\\';
	emsg[j] = msg[i];
	break;
      
      case '\b':
	emsg[j++] = '\\';
	emsg[j] = 'b';
	break;
	
      case '\r':
	emsg[j++] = '\\';
	emsg[j] = 'r';
	break;
	
      case '\f':
	emsg[j++] = '\\';
	emsg[j] = 'f';
	break;
	
      case '\n':
	emsg[j++] = '\\';
	emsg[j] = 'n';
	break;
	
      case '\t':
	emsg[j++] = '\\';
	emsg[j] = 't';
	break;
	
      default:
	emsg[j] = msg[i];
      }
    } else {
      //
      // Non-ASCII utf-8, we hope.
      //
      badutf8 = 0;
      mask    = 0x40;
      for( k=0; k<6 && ((msg[i] & mask) == mask) && i+k < sizeof(msg)-1 && j+k < sizeof(emsg)-1 ; k++, mask >>= 1) {
	if( k==0) {
	  emsg[j] = msg[i];
	} else if( (msg[i+k] & 0xe0) == 0x80) {
	  emsg[j+k] = msg[i+k];
	} else {
	  badutf8 = 1;
	  lslogging_log_message( "lsredis_sendStatusReport: non-UTF-8 character detected %s", msg);
	  return;
	}
      }
      if( badutf8 || (msg[i] & mask) == mask) {
	emsg[j] = 0;	// erase what we've added
	i += k;         // skip over the source characters that we do not understand
	continue;
      }
      j += k;
      i += k;
    }
    emsg[j+1] = 0;
  }

  lsredis_setstr( messp, "{\"severity\": %d, \"msg\": \"%s\"}", severity, emsg);
}


void lsredis_configCB( redisAsyncContext *ac, void *reply, void *privdata) {
  static const char *id = "lsredis_configCB";
  redisReply *r, *r2, *r3;
  int i;
  char *errmsg;
  int err, nerrmsg;

  r = reply;

  if( r == NULL) {
    lslogging_log_message( "%s: null reply, cannot configure, bad things will happen", id);
    return;
  }

  if( r->type != REDIS_REPLY_ARRAY || (r->elements % 2) != 0) {
    lslogging_log_message( "%s: could not understand config reply.  Bad things will happen.", id);
    return;
  }

  pthread_mutex_lock( &lsredis_config_mutex);

  for( i=0; i<r->elements; i += 2) {
    r2 = r->element[i];
    r3 = r->element[i+1];
    if( r2->type != REDIS_REPLY_STRING || r2->type != REDIS_REPLY_STRING)
      continue;
    //
    // the LHS of the redis keys for this station
    //
    if( strcmp( r2->str, "HEAD")==0) {
      lsredis_head = strdup( r3->str);
    }
    //
    // Publish changes to keys with this name as the publisher
    //
    if( strcmp( r2->str, "PUB")==0) {
      lsredis_publisher = strdup( r3->str);
    }

    if( strcmp( r2->str, "PG")==0) {
      pgpmac_use_pg = r3->str[0] == '0' ? 0 : 1;
    }

    if( strcmp( r2->str, "AUTOSCINT")==0) {
      pgpmac_use_autoscint = r3->str[0] == '0' ? 0 : 1;
    }

    if (strcmp(r2->str, "DETECTOR_STATE_MACHINE")==0) {
      pgmac_use_detector_state_machine = r3->str[0] == '1' ? 1 : 0;
    }

    if (strcmp(r2->str, "MONITOR_DETECTOR_POSITION")==0) {
      pgpmac_monitor_detector_position = r3->str[0] == '0' ? 0 : 1;
    }

    //
    // reg expression to select keys we will be keeping a local copy of
    // If we don't have a filter
    //
    if (strcmp(r2->str, "RE") == 0) {
      if (lsredis_key_select_regex.re_nsub > 0) {
	regfree(&lsredis_key_select_regex); // cleanup old compiled regex
      }
      err = regcomp(&lsredis_key_select_regex, r3->str, REG_EXTENDED);
      if (err != 0) {
	nerrmsg = regerror(err, &lsredis_key_select_regex, NULL, 0);
	if (nerrmsg > 0) {
	  errmsg = calloc(nerrmsg, sizeof(char));
	  nerrmsg = regerror(err, &lsredis_key_select_regex, errmsg, nerrmsg);
	  lslogging_log_message("lsredis_configCB: regcomp(%s) failed, %s",
				r3->str, errmsg);
	  free(errmsg);
	}
	exit(1);
      }
    }
  }

  /*
    2013-02-16 12:03:20.669342  ARRAY of 6 elements
    2013-02-16 12:03:20.669351      STRING: HEAD
    2013-02-16 12:03:20.669354      STRING: stns.2
    2013-02-16 12:03:20.669355      STRING: RE
    2013-02-16 12:03:20.669361      STRING: redis\.kvseq|stns\.2\.(.+)
    2013-02-16 12:03:20.669362      STRING: PUB
    2013-02-16 12:03:20.669364      STRING: MD2-21-ID-E
  */


  if( redisAsyncCommand( subac, lsredis_subCB, NULL, "PSUBSCRIBE REDIS_PV_CONNECTOR REDIS_PG_CONNECTOR REDIS_NODE_CONNECTOR mk_pgpmac_redis UI* MD2-* DETECTOR-21-ID-*") == REDIS_ERR) {
    lslogging_log_message( "Error sending PSUBSCRIBE command");
  }
  redisAsyncCommand( roac, lsredis_keysCB, NULL, "KEYS *");

  pthread_cond_signal( &lsredis_config_cond);
  pthread_mutex_unlock( &lsredis_config_mutex);
}


/** Initialize this module, that is, set up the connections
 *  \param pub  Publish under this (unique) name
 *  \param re   Regular expression to select keys we want to mirror
 *  \param head Prepend this (+ a dot) to the beginning of requested objects
 */
void lsredis_init() {
  static const char *id = "lsredis_init";
  int err;


  //
  // set up hash map to store redis objects
  //
  err = hcreate_r( 8192, &lsredis_htab);
  if( err == 0) {
    lslogging_log_message( "%s: Cannot create hash table.  Really bad things are going to happen.  hcreate_r returned %d", id, err);
  }

  pthread_cond_init( &lsredis_cond, NULL);

  subac = redisAsyncConnect("127.0.0.1", 6379);
  if( subac->err) {
    lslogging_log_message( "Error: %s", subac->errstr);
  }

  subfd.fd           = subac->c.fd;
  subfd.events       = 0;
  subac->ev.data     = &subfd;
  subac->ev.addRead  = lsredis_addRead;
  subac->ev.delRead  = lsredis_delRead;
  subac->ev.addWrite = lsredis_addWrite;
  subac->ev.delWrite = lsredis_delWrite;
  subac->ev.cleanup  = lsredis_cleanup;

  roac = redisAsyncConnect("127.0.0.1", 6379);
  if( roac->err) {
    lslogging_log_message( "%s: Error: %s", id, roac->errstr);
  }

  rofd.fd           = roac->c.fd;
  rofd.events       = 0;
  roac->ev.data     = &rofd;
  roac->ev.addRead  = lsredis_addRead;
  roac->ev.delRead  = lsredis_delRead;
  roac->ev.addWrite = lsredis_addWrite;
  roac->ev.delWrite = lsredis_delWrite;
  roac->ev.cleanup  = lsredis_cleanup;

  wrac = redisAsyncConnect("127.0.0.1", 6379);
  if( wrac->err) {
    lslogging_log_message( "Error: %s", wrac->errstr);
  }

  wrfd.fd           = wrac->c.fd;
  wrfd.events       = 0;
  wrac->ev.data     = &wrfd;
  wrac->ev.addRead  = lsredis_addRead;
  wrac->ev.delRead  = lsredis_delRead;
  wrac->ev.addWrite = lsredis_addWrite;
  wrac->ev.delWrite = lsredis_delWrite;
  wrac->ev.cleanup  = lsredis_cleanup;


  // separate hash table for the presets
  //
  hcreate_r( lsredis_preset_max_n * 2, &lsredis_preset_ht);

  pthread_mutexattr_init( &mutex_initializer);
  pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);

  pthread_mutex_init( &lsredis_mutex, &mutex_initializer);
  pthread_mutex_init( &lsredis_preset_list_mutex, &mutex_initializer);
  pthread_mutex_init( &lsredis_config_mutex, &mutex_initializer);
  pthread_cond_init(  &lsredis_config_cond,  NULL);

}

void lsredis_config() {
  static const char *id = "lsredis_config";
  char hostname[128], lhostname[128];
  int i;

  if( gethostname( hostname, sizeof(hostname)-1)) {
    lslogging_log_message( "%s: cannot get our own host name.  Cannot configure redis variables.", id);
  } else {
    for( i=0; i<strlen(hostname); i++) {
      lhostname[i] = tolower( hostname[i]);
    }
    lhostname[i] = 0;

    lslogging_log_message( "%s: our host name is '%s'", id, lhostname);
    redisAsyncCommand( roac, lsredis_configCB, NULL, "hgetall config.%s", lhostname);
  }
  
  pthread_mutex_lock( &lsredis_config_mutex);
  while( lsredis_head == NULL)
    pthread_cond_wait( &lsredis_config_cond, &lsredis_config_mutex);
  pthread_mutex_unlock( &lsredis_config_mutex);

}


/** Return the head string
 *  But only after we've read it.
 *  
 */
const char *lsredis_get_head() {
  const char *id = "lsredis_get_head";

  (void) (id);

  pthread_mutex_lock( &lsredis_config_mutex);
  while( lsredis_head == NULL) {
    pthread_cond_wait( &lsredis_config_cond, &lsredis_config_mutex);
  }
  pthread_mutex_unlock( &lsredis_config_mutex);

  return(lsredis_head);
}


/** service the socket requests
 */
void lsredis_fd_service( struct pollfd *evt) {
  pthread_mutex_lock( &lsredis_mutex);
  if( evt->fd == subac->c.fd) {
    if( evt->revents & POLLIN)
      redisAsyncHandleRead( subac);
    if( evt->revents & POLLOUT)
      redisAsyncHandleWrite( subac);
  }
  if( evt->fd == roac->c.fd) {
    if( evt->revents & POLLIN)
      redisAsyncHandleRead( roac);
    if( evt->revents & POLLOUT)
      redisAsyncHandleWrite( roac);
  }
  if( evt->fd == wrac->c.fd) {
    if( evt->revents & POLLIN)
      redisAsyncHandleRead( wrac);
    if( evt->revents & POLLOUT)
      redisAsyncHandleWrite( wrac);
  }
  pthread_mutex_unlock( &lsredis_mutex);
}


void lsredis_sig_service(
		      struct pollfd *evt		/**< [in] The pollfd object that triggered this call	*/
		      ) {
  struct signalfd_siginfo fdsi;

  //
  // Really, we don't care about the signal,
  // it's just used to drop out of the poll
  // function when there is something for us
  // to do.
  //


  read( evt->fd, &fdsi, sizeof( struct signalfd_siginfo));

}

void lsredis_heartbeat_cb( char *event) {
  static lsredis_obj_t *hb_time = NULL;
  struct timespec now;
  struct tm lnow;
  char snow[64];
  unsigned int msecs;

  if( hb_time == NULL) {
    hb_time = lsredis_get_obj( "time");
  }

  if( hb_time == NULL) {
    lslogging_log_message( "lsredis_heartbeat_cb: null redis object.  Why?");
    return;
  }

  clock_gettime( CLOCK_REALTIME, &now);
  localtime_r( &now.tv_sec, &lnow);
  msecs = now.tv_nsec / 1000;
  strftime( snow, sizeof(snow)-1, "%Y-%m-%d %H:%M:%S", &lnow);
  lsredis_setstr( hb_time, "%s.%.06u", snow, msecs);
}


/** subscribe to changes and service sockets
 */
void *lsredis_worker(  void *dummy) {
  static int poll_timeout_ms = -1;	//!< poll timeout, in millisecs (of course)
  static struct pollfd fda[4];		//!< array of pollfd's for the poll function, one entry per connection
  static int nfda = 0;			//!< number of active elements in fda
  static sigset_t our_sigset;
  int pollrtn;
  int i;


  pthread_mutex_lock( &lsredis_mutex);
  //
  // block ordinary signal mechanism
  //
  sigemptyset( &our_sigset);
  sigaddset( &our_sigset, SIGUSR1);
  pthread_sigmask( SIG_BLOCK, &our_sigset, NULL);

  // Set up fd mechanism
  //
  fda[0].fd = signalfd( -1, &our_sigset, SFD_NONBLOCK);
  if( fda[0].fd == -1) {
    char *es;

    es = strerror( errno);
    lslogging_log_message( "lsredis_worker: Signalfd trouble '%s'", es);
  }
  fda[0].events = POLLIN;
  nfda = 1;

  lsredis_running = 1;

  pthread_cond_signal( &lsredis_cond);
  pthread_mutex_unlock( &lsredis_mutex);

  lsevents_add_listener( "^Heartbeat$", lsredis_heartbeat_cb);
  lstimer_set_timer( "Heartbeat", -1, 1, 0);

  while(1) {
    nfda = 1;

    pthread_mutex_lock( &lsredis_mutex);
    if( subfd.fd != -1) {
      fda[nfda].fd      = subfd.fd;
      fda[nfda].events  = subfd.events;
      fda[nfda].revents = 0;
      nfda++;
    }

    if( rofd.fd != -1) {
      fda[nfda].fd      = rofd.fd;
      fda[nfda].events  = rofd.events;
      fda[nfda].revents = 0; 
     nfda++;
    }

    if( wrfd.fd != -1) {
      fda[nfda].fd      = wrfd.fd;
      fda[nfda].events  = wrfd.events;
      fda[nfda].revents = 0;
      nfda++;
    }
    pthread_mutex_unlock( &lsredis_mutex);

    pollrtn = poll( fda, nfda, poll_timeout_ms);

    if( pollrtn && fda[0].revents) {
      lsredis_sig_service( &(fda[0]));
      pollrtn--;
    } 

    for( i=1; i<nfda; i++) {
      if( fda[i].revents) {
        lsredis_fd_service( &(fda[i]));
      }
    }
  }
}

pthread_t *lsredis_run() {
  pthread_create( &lsredis_thread, NULL, lsredis_worker, NULL);
  return &lsredis_thread;
}
