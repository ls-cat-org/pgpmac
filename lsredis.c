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
</pre>
 * (this last value is used to support the now depreciated px.kvs table in the LS-CAT postgresql server).
 * We assume that all publisher that we are listening to ONLY publish key names that have changed.
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
 *   
 */

static pthread_t lsredis_thread;

static pthread_mutex_t lsredis_mutex;
static pthread_cond_t  lsredis_cond;
static int lsredis_running = 0;


static lsredis_obj_t *lsredis_objs = NULL;
static struct hsearch_data lsredis_htab;

static redisAsyncContext *subac;
static redisAsyncContext *roac;
static redisAsyncContext *wrac;

static char *lsredis_publisher = NULL;
static regex_t lsredis_key_select_regex;
static char *lsredis_head = NULL;

static struct pollfd subfd;
static struct pollfd rofd;
static struct pollfd wrfd;

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

  if( !(p->valid)) {
    p->valid = 1;
    lsevents_send_event( "%s Valid", p->events_name);
  }
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
  int rtn;

  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0) 
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = regexec( preg, p->value, nmatch, pmatch, eflags);

  pthread_mutex_unlock( &p->mutex);

  return rtn;
}

/** return a copy of the key's string value
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
  if( p->valid && strcmp( v, p->value) == 0) {
    // nothing to do
    pthread_mutex_unlock( &p->mutex);
    return;
  }

  p->wait_for_me++;						//!< up the count of times we need to see ourselves published before we start listening to others again


  argv[0] = "HSET";
  argv[1] = p->key;
  argv[2] = "VALUE";
  argv[3] = v;		//!< redisAsyncCommandArgv shouldn't need to access this after it's made up it's packet (before it returns) so we should be OK with this location disappearing soon.

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
  _lsredis_set_value( p, v);
  pthread_cond_signal( &p->cond);
  pthread_mutex_unlock( &p->mutex);
}


double lsredis_getd( lsredis_obj_t *p) {
  double rtn;

  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->dvalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}

long int lsredis_getl( lsredis_obj_t *p) {
  long int rtn;

  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->lvalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}  

char **lsredis_get_string_array( lsredis_obj_t *p) {
  char **rtn;

  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->avalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}

int lsredis_getb( lsredis_obj_t *p) {
  int rtn;

  pthread_mutex_lock( &p->mutex);
  while( p->valid == 0)
    pthread_cond_wait( &p->cond, &p->mutex);

  rtn = p->bvalue;
  pthread_mutex_unlock( &p->mutex);
  
  return rtn;
}  

char lsredis_getc( lsredis_obj_t *p) {
  int rtn;

  pthread_mutex_lock( &p->mutex);
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

  //  lslogging_log_message( "hgetCB: %s %s", p == NULL ? "<NULL>" : p->key, r->type == REDIS_REPLY_STRING ? r->str : "Non-string value.  Why?");

  //
  // Apparently this item does not exist
  // Just set it to an empty string so at least other apps will have the same behaviour as us
  // TODO: figure out a better way to deal with missing key/values
  //
  if( p != NULL && r->type == REDIS_REPLY_NIL) {
    lsredis_setstr( p, "");
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
    
    err = regexec( &lsredis_key_select_regex, key, 2, pmatch, 0);
    if( err == 0 && pmatch[1].rm_so != -1) {
      p->events_name = strndup( key+pmatch[1].rm_so, pmatch[1].rm_eo - pmatch[1].rm_so);
    } else {
      p->events_name = strdup( key);
    }
    if( p->events_name == NULL) {
      lslogging_log_message( "_lsredis_get_obj: Out of memory (events_name)");
      exit( -1);
    }

    pthread_mutex_init( &p->mutex, NULL);
    pthread_cond_init(  &p->cond, NULL);
    p->value = NULL;
    p->valid = 0;
    lsevents_send_event( "%s Invalid", p->events_name);
    p->wait_for_me = 0;
    p->key = strdup( key);
    p->hits = 0;
  
    htab_input.key  = p->key;
    htab_input.data = p;

    errno = 0;
    err = hsearch_r( htab_input, ENTER, &htab_output, &lsredis_htab);
    if( err == 0) {
      lslogging_log_message( "_lsredis_get_obj: hseach error on enter.  errno=%d", errno);
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
  lsredis_obj_t *rtn;
  va_list arg_ptr;
  char k[512];
  char *kp;
  int nkp;

  va_start( arg_ptr, fmt);
  vsnprintf( k, sizeof(k)-1, fmt, arg_ptr);
  k[sizeof(k)-1] = 0;
  va_end( arg_ptr);

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
  if( regexec( &lsredis_key_select_regex, k, 0, NULL, 0) == 0) {
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
	// Don't get a new value, either we set it last or we are still waiting for redis to report
	// our publication
	//
	return;
      }

      // Here we know our value is out of date
      //
      p->valid = 0;
      lsevents_send_event( "%s Invalid", p->events_name);
      pthread_mutex_unlock( &p->mutex);

      //
      // We shouldn't get here if wait_for_me is zero and we are the publisher.
      // If somehow we did (ie we did an hset with out incrementing wait_for_me or if we published too many times), it shouldn't hurt to get the value again.
      //
      redisAsyncCommand( roac, lsredis_hgetCB, p, "HGET %s VALUE", k);
    }
  }
}


void lsredis_maybe_add_key( char *k) {
  if( regexec( &lsredis_key_select_regex, k, 0, NULL, 0) == 0) {
    _lsredis_get_obj( k);
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
      lslogging_log_message( "lsredis_keysCB: exected string...");
      lsredis_debugCB( ac, r->element[i], privdata);
    } else {
      lsredis_maybe_add_key( r->element[i]->str);
    }
  }
}

int lsredis_find_preset( char *base, char *preset_name, double *dval) {
  char s[512];
  int i;
  int err;
  ENTRY htab_input, *htab_output;
  lsredis_obj_t *p;

  i = 0;
  for( i=0; i<1024; i++) {
    snprintf( s, sizeof( s)-1, "%s.%s.presets.%d.name", lsredis_head, base, i);
    s[sizeof(s)-1] = 0;
    htab_input.key  = s;
    htab_input.data = NULL;
    err = hsearch_r( htab_input, FIND, &htab_output, &lsredis_htab);
    if( err == 0) {
      // We've run out of names to look for: done
      lslogging_log_message( "lsredis_find_preset: no preset for motor %s named '%s'", base, preset_name);
      *dval = 0.0;
      return 0;
    }

    // Check if we have a match
    p = htab_output->data;
    if( lsredis_cmpstr( p, preset_name)==0) {
      // got a match, now look for the position
      snprintf( s, sizeof( s)-1, "%s.%s.presets.%d.position", lsredis_head, base, i);
      s[sizeof(s)-1] = 0;
      htab_input.key = s;
      htab_input.data = NULL;
      err = hsearch_r( htab_input, FIND, &htab_output, &lsredis_htab);
      if( err == 0) {
	// Name but not position? odd.
	lslogging_log_message( "lsredis_find_preset: Error, motor %s preset '%s' has no position defined", base, preset_name);
	*dval = 0.0;
	return 0;
      }
      p = htab_output->data;
      *dval = lsredis_getd( p);
      return 1;
    }
  }
  // How'd we get here?
  // did someone really define that many presets?  And then looked for one that's not there?
  *dval = 0;
  return 0;
}



/** Initialize this module, that is, set up the connections
 *  \param pub  Publish under this (unique) name
 *  \param re   Regular expression to select keys we want to mirror
 *  \param head Prepend this (+ a dot) to the beginning of requested objects
 */
void lsredis_init( char *pub, char *re, char *head) {
  int err;
  int nerrmsg;
  char *errmsg;

  //
  // set up hash map to store redis objects
  //
  err = hcreate_r( 8192, &lsredis_htab);
  if( err == 0) {
    lslogging_log_message( "lsredis_init: Cannot create hash table.  Really bad things are going to happen.  hcreate_r returnd %d", err);
  }

  lsredis_head      = strdup( head);
  lsredis_publisher = strdup( pub);

  pthread_mutex_init( &lsredis_mutex, NULL);
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
    lslogging_log_message( "Error: %s", roac->errstr);
  }

  rofd.fd           = roac->c.fd;
  rofd.events       = 0;
  roac->ev.data     = &rofd;
  roac->ev.addRead  = lsredis_addRead;
  roac->ev.delRead  = lsredis_delRead;
  roac->ev.addWrite = lsredis_addWrite;
  roac->ev.delWrite = lsredis_delWrite;
  roac->ev.cleanup  = lsredis_cleanup;

  //wrac = redisAsyncConnect("10.1.0.3", 6379);
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

  err = regcomp( &lsredis_key_select_regex, re, REG_EXTENDED);
  if( err != 0) {
    nerrmsg = regerror( err, &lsredis_key_select_regex, NULL, 0);
    if( nerrmsg > 0) {
      errmsg = calloc( nerrmsg, sizeof( char));
      nerrmsg = regerror( err, &lsredis_key_select_regex, errmsg, nerrmsg);
      lslogging_log_message( "lsredis_select: %s", errmsg);
      free( errmsg);
    }
  }
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

  if( redisAsyncCommand( subac, lsredis_subCB, NULL, "PSUBSCRIBE REDIS_KV_CONNECTOR UI* MD2-*") == REDIS_ERR) {
    lslogging_log_message( "Error sending PSUBSCRIBE command");
  }

  redisAsyncCommand( roac, lsredis_keysCB, NULL, "KEYS *");
  pthread_cond_signal( &lsredis_cond);
  pthread_mutex_unlock( &lsredis_mutex);


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


void lsredis_run() {
  pthread_create( &lsredis_thread, NULL, lsredis_worker, NULL);
}
