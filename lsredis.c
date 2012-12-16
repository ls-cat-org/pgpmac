#include "pgpmac.h"
/*! \file lsredis.c
 *  \brief Support redis hash synchronization
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */

static pthread_t lsredis_thread;

static lsredis_obj_t *lsredis_objs = NULL;
static pthread_mutex_t lsredis_objs_mutex;
static pthread_mutex_t lsredis_ro_mutex;	//!< keep from having more than one thread send a rediscommand to the read/only  server
static pthread_mutex_t lsredis_wr_mutex;	//!< keep from having more than one thread send a rediscommand to the write/read server

static redisAsyncContext *subac;
static redisAsyncContext *roac;
static redisAsyncContext *wrac;

static char *lsredis_publisher = NULL;
static regex_t lsredis_key_select_regex;
static char *lsredis_head = NULL;

static struct pollfd subfd;
static struct pollfd rofd;
static struct pollfd wrfd;

/** set_value and setstr helper funciton
 *  p->mutex must be locked before calling
 */
void _lsredis_set_value( lsredis_obj_t *p, char *v) {

  if( strlen(v) >= p->value_length) {
    if( p->value != NULL)
      free( p->value);
    p->value_length = strlen(v) + 256;
    p->value = calloc( p->value_length, sizeof( char));
    if( p->value == NULL) {
      lslogging_log_message( "_lsredis_set_value: out of memory");
      exit( -1);
    }
  }
  strcpy( p->value, v);
  p->value[p->value_length-1] = 0;
  p->dvalue = strtod( p->value, NULL);
  p->lvalue = strtol( p->value, NULL, 10);
  
  if( p->avalue != NULL) {
    char **zz;
    for( zz = p->avalue; *zz != NULL; zz++)
      free( zz);
    free( p->avalue);
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
	p->bvalue = -1;		// a little unusual for a null value to be -1
    }

  p->valid = 1;

  lsevents_send_event( "%s Valid", p->events_name);
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
  
  pthread_mutex_lock(   &p->mutex);

  if( p->valid && strcmp( v, p->value) == 0) {
    // nothing to do
    pthread_mutex_unlock( &p->mutex);
    return;
  }

  p->valid       = 0;						//!< invalidate the current value: set_value will fix this and signal waiting threads
  lsevents_send_event( "%s Invalid", p->events_name);
  p->wait_for_me++;						//!< up the count of times we need to see ourselves published before we start listening to others again


  argv[0] = "HSET";
  argv[1] = p->key;	//!< key is "immutable" (not really a C feature).  In any case no one is going to be changing it so it's cool to read it without mutex protection.
  argv[2] = "VALUE";
  argv[3] = v;		//!< redisAsyncCommandArgv shouldn't need to access this after it's made up it's packet (before it returns) so we should be OK with this location disappearing soon.

  pthread_mutex_lock( &lsredis_wr_mutex);
  redisAsyncCommand( wrac, NULL, NULL, "MULTI");
  redisAsyncCommandArgv( wrac, NULL, NULL, 4, (const char **)argv, NULL);

  redisAsyncCommand( wrac, NULL, NULL, "PUBLISH %s %s", lsredis_publisher, p->key);
  redisAsyncCommand( wrac, NULL, NULL, "EXEC");
  pthread_mutex_unlock( &lsredis_wr_mutex);

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

void lsredis_hgetCB( redisAsyncContext *ac, void *reply, void *privdata) {
  redisReply *r;
  lsredis_obj_t *p;

  r = reply;
  p =  privdata;

  lslogging_log_message( "hgetCB: %s %s", p == NULL ? "<NULL>" : p->key, r->type == REDIS_REPLY_STRING ? r->str : "Non-string value.  Why?");

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
 */
lsredis_obj_t *_lsredis_get_obj( char *key) {
  lsredis_obj_t *p;
  regmatch_t pmatch[2];
  int err;
  char *name;

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

  //  printf( "_lsredis_get_obj: received key '%s'", key);
  //  fflush( stdout);

  pthread_mutex_lock( &lsredis_objs_mutex);
  // If the key is already there then just return it
  //
  for( p = lsredis_objs; p != NULL; p = p->next) {
    if( strcmp( key, p->key) == 0) {
      break;
    }
  }

  if( p != NULL) {
    pthread_mutex_unlock( &lsredis_objs_mutex);
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
      lslogging_log_message( "_lsredis_get_obj: Out of memory (evetns_name)");
      exit( -1);
    }

    pthread_mutex_init( &p->mutex, NULL);
    pthread_cond_init(  &p->cond, NULL);
    p->valid = 0;
    lsevents_send_event( "%s Invalid", p->events_name);
    p->wait_for_me = 0;
    p->key = strdup( key);
    p->getstr = lsredis_getstr;
    p->getd = lsredis_getd;
    p->getl = lsredis_getl;
    p->hits = 0;
  
    p->next = lsredis_objs;
    lsredis_objs = p;

    pthread_mutex_unlock( &lsredis_objs_mutex);

    lslogging_log_message( "_lsredis_get_obj: added %s", key);
  }
  //
  // We arrive here with the valid flag lowered.  Go ahead and request the latest value.
  //
  pthread_mutex_lock( &lsredis_ro_mutex);
  redisAsyncCommand( roac, lsredis_hgetCB, p, "HGET %s VALUE", key);
  pthread_mutex_unlock( &lsredis_ro_mutex);

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

  nkp = strlen(k) + strlen( lsredis_head) + 16;		// 16 is overkill, I know.  get over it.
  kp = calloc( nkp, sizeof( char));
  if( kp == NULL) {
    lslogging_log_message( "lsredis_get_obj: Out of memory");
    exit( -1);
  }
  
  snprintf( kp, nkp-1, "%s.%s", lsredis_head, k);
  kp[nkp-1] = 0;
  rtn = _lsredis_get_obj( kp);
  free( kp);
  return rtn;
}

/** call back incase a redis server becomes disconnected
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
  pfd->events |= POLLIN;
}

/** hook to manage "don't need to read" events
 */
void lsredis_delRead( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events &= ~POLLIN;
}

/** hook to manage write events
 */
void lsredis_addWrite( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events |= POLLOUT;
}

/** hook to manage "don't need to write anymore" events
 */
void lsredis_delWrite( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events &= ~POLLOUT;
}

/** hook to clean up
 * TODO: figure out what we are supposed to do here and do it
 */
void lsredis_cleanup( void *data) {
  struct pollfd *pfd;
  pfd = (struct pollfd *)data;
  pfd->events &= ~(POLLOUT | POLLIN);
  pfd->fd = -1;
}


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
    for( i=0; i<r->elements; i++)
      lsredis_debugCB( ac, r->element[i], NULL);
    indentlevel--;
    break;

  default:
    lslogging_log_message( "%*sUnknown type %d", indentlevel*4,"", r->type);

  }
}




/** Use the publication to request the new value
 */
void lsredis_subCB( redisAsyncContext *ac, void *reply, void *privdata) {
  redisReply *r;
  lsredis_obj_t *p, *last, *last2;
  char *k;
  char *publisher;

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
    pthread_mutex_lock( &lsredis_objs_mutex);
    last  = NULL;
    last2 = NULL;
    for( p=lsredis_objs; p != NULL; p = p->next) {
      if( strcmp( p->key, k) == 0) {
	p->hits++;
	//
	// Maybe reorder our list so the most often updated objects
	// eventually bump up to the beginning of the list.
	// That "hits+4" keeps us from oscillating when objects are accessed equally
	//
	if( last != NULL && last->hits < p->hits+4) {
	  last->next = p->next;
	  p->next    = last;
	  if( last2 != NULL)
	    last2->next = p;
	  else
	    lsredis_objs = p;
	}
	break;
      }
      last2 = last;
      last  = p;
    }    
    pthread_mutex_unlock( &lsredis_objs_mutex);

    if( p == NULL) {
      //
      // Regardless of who the publisher is, apparently there is a key we've not seen before
      //
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
      pthread_mutex_lock( &lsredis_ro_mutex);
      redisAsyncCommand( roac, lsredis_hgetCB, p, "HGET %s VALUE", k);
      pthread_mutex_unlock( &lsredis_ro_mutex);
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
  
  for( i=0; i<r->elements; i++) {
    if( r->element[i]->type != REDIS_REPLY_STRING) {
      lslogging_log_message( "lsredis_keysCB: exected string...");
      lsredis_debugCB( ac, r->element[i], privdata);
    } else {
      lsredis_maybe_add_key( r->element[i]->str);
    }
  }
}



/** set regexp to select variables we are interested in following
 *
 *  Note that redis only supports glob matching while we'd prefer something a tad more
 *  useful.  See http://xkcd.com/208
 *
 */
void lsredis_select( char *re) {
  int err;
  char *errmsg;
  int  nerrmsg;

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
  
  pthread_mutex_lock( &lsredis_ro_mutex);
  redisAsyncCommand( roac, lsredis_keysCB, NULL, "KEYS *");
  pthread_mutex_unlock( &lsredis_ro_mutex);
}

/** Initialize this module, that is, set up the connections
 *  \param pub  Publish under this (unique) name
 *  \param re   Regular expression to select keys we want to mirror
 *  \param head Prepend this (+ a dot) to the beginning of requested objects
 */
void lsredis_init( char *pub, char *re, char *head) {

  lsredis_head = strdup( head);
  lsredis_publisher = strdup( pub);

  pthread_mutex_init( &lsredis_objs_mutex, NULL);
  pthread_mutex_init( &lsredis_ro_mutex, NULL);
  pthread_mutex_init( &lsredis_wr_mutex, NULL);

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

  wrac = redisAsyncConnect("10.1.0.3", 6379);
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

  lsredis_select( re);
}


/** service the socket requests
 */
void lsredis_fd_service( struct pollfd *evt) {
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
}


/** subscribe to changes and service sockets
 */
void *lsredis_worker(  void *dummy) {
  static struct pollfd fda[3];		//!< array of pollfd's for the poll function, one entry per connection
  static int nfda = 0;			//!< number of active elements in fda
  static int poll_timeout_ms = -1;	//!< poll timeout, in millisecs (of course)
  int pollrtn;
  int i;

  pthread_mutex_lock( &lsredis_ro_mutex);
  if( redisAsyncCommand( subac, lsredis_subCB, NULL, "PSUBSCRIBE REDIS_KV_CONNECTOR UI*") == REDIS_ERR) {
    lslogging_log_message( "Error sending PSUBSCRIBE command");
  }
  pthread_mutex_unlock( &lsredis_ro_mutex);

  
  while(1) {
    nfda = 0;

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

    pollrtn = poll( fda, nfda, poll_timeout_ms);

    for( i=0; i<nfda; i++) {
      if( fda[i].revents) {
        lsredis_fd_service( &(fda[i]));
      }
    }
  }
}


void lsredis_run() {
  pthread_create( &lsredis_thread, NULL, lsredis_worker, NULL);
}
