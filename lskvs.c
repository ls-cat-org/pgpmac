#include "pgpmac.h"
/*! \file lskvs.c
 *  \brief Support for the remote access client key value pairs
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */



lskvs_kvs_t *lskvs_kvs = NULL;	//!< our list (or at least the start of it
pthread_rwlock_t lskvs_rwlock;		//!< needed to protect the list


/** find a postion for a given preset name
 *
 * \param mp Motor pointer
 * \param name The preset to search for
 * \param err set to non-zero on error, ignored if null
 */
double lskvs_find_preset_position( lspmac_motor_t *mp, char *name, int *err) {
  regmatch_t pmatch[4], qmatch[4];
  double rtn;
  lskvs_kvs_list_t
    *position_kv = NULL,
    *name_kv     = NULL;
  int e;

  *err = -4;
  if( name == NULL || *name == 0)
    return 0.0;

  *err = 0;
  for( name_kv = mp->presets; name_kv != NULL; name_kv = name_kv->next) {
    if( strcmp( name, name_kv->kvs->v) == 0) {
      //
      // We found the correct preset, now get the index
      //
      e = regexec( &(mp->preset_regex), name_kv->kvs->k, 4, pmatch, 0);
      if( e != 0) {
	lslogging_log_message( "lskvs_find_preset_position: could not parse name key '%s'", name_kv->kvs->k);
	if( err != NULL)
	  *err = e;
	return 0.0;
      }

      for( position_kv = mp->presets; position_kv != NULL; position_kv = position_kv->next) {
	if( position_kv == name_kv)
	  continue;

	e = regexec( &(mp->preset_regex), position_kv->kvs->k, 4, qmatch, 0);
	if( e != 0) {
	  lslogging_log_message( "lskvs_find_preset_position: could not parse position key '%s'", position_kv->kvs->k);
	  if( err != NULL)
	    *err = e;
	  return 0.0;
	}

	if( strncmp( name_kv->kvs->k, position_kv->kvs->k, qmatch[2].rm_eo + 1) == 0) {
	  break;
	}
      }
      if( position_kv != NULL)
	break;
    }
  }

  if( name_kv != NULL || position_kv != NULL) {
    errno = 0;
    rtn = strtod( position_kv->kvs->v, NULL);
    if( errno != 0) {
      lslogging_log_message( "lskvs_find_preset_position: bad preset value for motor %s, preset %s, value '%s'", mp->name, name, position_kv->kvs->v);
      if( err != NULL)
	*err = -2;
      return 0.0;
    }
    return rtn;
  }
  lslogging_log_message( "lskvs_find_preset_position: could not find preset for motor %s, preset %s", mp->name, name);
  if( err != NULL)
    *err = -3;
  return 0.0;
}


/** Utility wrapper for regcomp providing printf style formating
 *  \param preg   Buffer for the compile regex object
 *  \param cflags See regcomp man page
 *  \param fmt    Printf style formating string
 *  \param ...    Argument list specified by fmt
 */
void lskvs_regcomp( regex_t *preg, int cflags, char *fmt, ...) {
  struct regerror_struct {
    int errcode;
    char *errstr;
  };
  static struct regerror_struct regerrors[] = {
    { REG_BADBR,    "Invalid use of back reference operator."},
    { REG_BADPAT,   "Invalid use of pattern operators such as group or list."},
    { REG_BADRPT,   "Invalid use of repetition operators such as using '*' as the first character."},
    { REG_EBRACE,   "Un-matched brace interval operators."},
    { REG_EBRACK,   "Un-matched bracket list operators."},
    { REG_ECOLLATE, "Invalid collating element."},
    { REG_ECTYPE,   "Unknown character class name."},
    { REG_EEND,     "Non specific error.  This is not defined by POSIX.2."},
    { REG_EESCAPE,  "Trailing backslash."},
    { REG_EPAREN,   "Un-matched parenthesis group operators."},
    { REG_ERANGE,   "Invalid use of the range operator, e.g., the ending point of the range occurs prior to the starting point."},
    { REG_ESIZE,    "Compiled regular expression requires a pattern buffer larger than 64Kb.  This is not defined by POSIX.2."},
    { REG_ESPACE,   "The regex routines ran out of memory."},
    { REG_ESUBREG,  "Invalid back reference to a subexpression."},
    { 0,            "No errors"}
  };



  va_list arg_ptr;
  char s[512];		//!< no reason our search strings should ever be this big
  int err;

  va_start( arg_ptr, fmt);
  vsnprintf( s, sizeof(s)-1, fmt, arg_ptr);
  s[ sizeof(s)-1] = 0;
  va_end( arg_ptr);

  err = regcomp( preg, s, cflags);
  if( err != 0) {
    int i;

    for( i=0; regerrors[i].errcode != 0; i++)
      if( regerrors[i].errcode == err)
	break;

    if( regerrors[i].errcode != 0) {
      lslogging_log_message( "lskvs_regcomp: could not compile regular experssion '%s'", s);
      lslogging_log_message( "lskvs_regcomp: regcomp returned %d: %s", err, regerrors[i]);
    }
  }
}


/** Set the value of a kv pair
 * Create the pair if the key does not exsist.
 *
 * If more than one thread tries to create the same key at the same time
 * it is possible for the list to contain multiple versions.  Not good.  But
 * also not possible if only one thread has the job of create the pairs in the first
 * place.  Alternatively just grab the write lock at the beginning 
 * and hold it until the end.  The advantage of having only one thread calling lskvs_set
 * is that it wont slow down the other threads that just want to read things.
 * In any case, we'll likely never see so much action for any of this to make a differene.
 *
 * \param k The name of the key
 * \param v The value to assign to the key
 */
void lskvs_set( char *k, char *v) {
  lskvs_kvs_t
    *root,
    *p;

  lslogging_log_message( "lskvs_set:  k: '%s', v: '%s'", k, v);

  // Don't bother with empty keys
  //
  if( k == NULL || *k == 0)
    return;

  pthread_rwlock_rdlock( &lskvs_rwlock);
  root = lskvs_kvs;
  pthread_rwlock_unlock( &lskvs_rwlock);

  for( p=root; p != NULL; p = p->next) {
    if( strcmp( p->k, k) == 0) {
      break;
    }
  }

  if( p == NULL) {
    //
    // Add a new list item
    //
    p = calloc( 1, sizeof( *p));
    if( p == NULL) {
      lslogging_log_message( "lskvs_set: out of memory for kv struct (%d bytes", sizeof( *p));
      exit( -1);
    }


    p->k = calloc( strlen(k)+1, sizeof( *k));
    if( p->k == NULL) {
      lslogging_log_message( "lskvs_set: out of memory for k (%d bytes)", strlen( k)+1);
      exit( -1);
    }
    strcpy( p->k, k);
    p->k[strlen(k)] = 0;

    // leave a little room to grow
    //
    if( v == NULL || *v == 0)
      p->vl = 32;
    else
      p->vl = strlen(v) + 32;

    p->v = calloc( p->vl, sizeof( *v));
    if( p->v == NULL) {
      lslogging_log_message( "lskvs_set: out of memory for v (%d bytes)", p->vl);
      exit( -1);
    }
    
    if( v == NULL || *v == 0)
      *(p->v) = 0;
    else
      strcpy( p->v, v);

    p->v[p->vl-1] = 0;
    
    pthread_rwlock_init( &p->l, NULL);

    pthread_rwlock_wrlock( &lskvs_rwlock);
    p->next   = lskvs_kvs;
    lskvs_kvs = p;
    pthread_rwlock_unlock( &lskvs_rwlock);

    lsevents_send_event( "NewKV");

  } else {
    //
    // Just update the value
    // Assume the database only sent us an update because
    // the old and new values are different
    //
    pthread_rwlock_wrlock( &(p->l));
    if( strlen( v) > p->vl-1) {
      free( p->v);
      
      p->vl = strlen(v) + 32;
      p->v = calloc( p->vl, 1);
      if( p->v == NULL) {
	lslogging_log_message( "lskvs_set: out of memory for re-calloc of v (%d bytes)", p->vl);
	exit( -1);
      }
    }
    strcpy( p->v, v);
    p->v[p->vl-1] = 0;
    pthread_rwlock_unlock( &(p->l));
  }
}

/** Find the kv pair object
 *  Return with a pointer to the structure or NULL if not found
 */
lskvs_kvs_t *lskvs_get(
		       char *k	//!< [in] key name to search for
		       ) {
  lskvs_kvs_t
    *rtn;

  pthread_rwlock_rdlock( &lskvs_rwlock);
  rtn = lskvs_kvs;
  pthread_rwlock_unlock( &lskvs_rwlock);

  while(rtn != NULL) {
    if( strcmp( rtn->k, k) == 0)
      break;
    rtn = rtn->next;
  }
  return rtn;
}


/** Initialize lskvs objects
 */
void lskvs_init() {
  pthread_rwlock_init( &lskvs_rwlock, NULL);
}

/** Run things.
 *  Really, there is nothing to run.  There is no need for a worker thread here
 *  but this has been added so we can add lskvs just like any other module to the pgpmac
 *  project.  Maybe one day we'll need to add a thread and this little routine can be
 *  celebrated as being far sighted, ahead of its time.
 */
void lskvs_run() {
}
