#include "pgpmac.h"
/*! \file lskvs.c
 *  \brief Support for the remote access client key value pairs
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */



/** Storage for the key value pairs
 *
 * the k's and v's are strings and to keep the memory management less crazy
 * we'll calloc some space for these strings and only free and re-calloc if we need
 * more space later.  Only the values are ever going to be resized.
 */
typedef struct lskvs_kvs_struct {
  struct lskvs_kvs_struct *next;	//!< the next kvpair
  pthread_rwlock_t l;			//!< our lock
  char *k;				//!< the key
  char *v;				//!< the value
  int   vl;				//!< the length of the calloced v
} lskvs_kvs_t;

static lskvs_kvs_t *lskvs_kvs = NULL;	//!< our list (or at least the start of it
static pthread_rwlock_t lskvs_rwlock;	//!< needed to protect the list



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
