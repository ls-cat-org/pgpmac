#include "pgpmac.h"
/** \file lsraster.c
 *  \brief Generate datasets for raster scanning
 *  \date 2017
 *  \author Keith Brister
 *  \copyright Northwestern University All Rights Reserved
 */

static pthread_t lsraster_thread;

static redisContext *lsraster_redisContext;
static const char *lsraster_head;
static pthread_mutex_t lsraster_mutex;
static pthread_cond_t  lsraster_cond;
static char *lsraster_current_key = NULL;
static int lsraster_working = 0;


/** initialize redis connection
 *
 *  This should come after the lsredis_init routine as we are going to
 *  be relying on it to get the prefix (aka head) needed to talk to
 *  our redis keys.  This relies on a mutex that gets initialized in
 *  lsredis_init.
 *
 *  "Why not just use the lsredis connections instead of inventing our
 *  own?" I hear you ask.  Well, lsredis is all set up to handle
 *  hashes in the special way that we do while for raster scans we
 *  apparently just need a few routines to handle talking to lists.
 *  We are on a separate thread anyway so using blocking calls
 *  shouldn't effect the rest of pgpmac's performance so we'll just
 *  use the non-async redis calls here to keep things simpler and
 *  easier to debug.
 */
void lsraster_init() {
  static const char *id = "lsraster_init";
  pthread_mutexattr_t mutex_initializer;

  pthread_mutexattr_init( &mutex_initializer);
  pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init( &lsraster_mutex, &mutex_initializer);
  pthread_cond_init(&lsraster_cond, NULL);

  lsraster_head = lsredis_get_head();

  lsraster_redisContext = redisConnect("127.0.0.1", 6379);
  if (lsraster_redisContext == NULL || lsraster_redisContext->err) {
    lslogging_log_message("%s: redisConnect failed %s", id, lsraster_redisContext ? lsraster_redisContext->errstr : "Allocation failed");
    return;
  }
}

void lsraster_step( const char *key) {
  static const char *id = "lsraster_step";

  (void)(id);

  pthread_mutex_lock(&lsraster_mutex);
  if (lsraster_current_key) {
    free(lsraster_current_key);
    lsraster_current_key = NULL;
  }
  lsraster_current_key = strdup(key);
  lsraster_working = 1;
  pthread_cond_signal(&lsraster_cond);
  pthread_mutex_unlock(&lsraster_mutex);

}

void *lsraster_worker(void *dummy) {
  static const char *id = "lsraster_worker";
  redisReply *reply, *reply2;

  while (1) {
    pthread_mutex_lock(&lsraster_mutex);
    while (lsraster_working == 0) {
      pthread_cond_wait(&lsraster_cond, &lsraster_mutex);
    }
    
    do {
      reply = redisCommand(lsraster_redisContext, "RPOPLPUSH %s %s_working", lsraster_current_key, lsraster_current_key);
      if (!reply) {
        lslogging_log_message("%s: RPOPLPUSH failed for key %s", id, lsraster_current_key);
        break;
      }
      if (reply->type == REDIS_REPLY_ERROR) {
        lslogging_log_message("%s: RPOPLPUSH failed for key %s: %s", id, lsraster_current_key, reply->str);
        break;
      }

      if (reply->type == REDIS_REPLY_NIL) {
        break;
      }

      lslogging_log_message("%s: params: %s", id, reply->str);
      
      lspg_query_push(NULL, NULL, "SELECT px.raster_step('%s'::jsonb)", reply->str);

      reply2 = redisCommand(lsraster_redisContext, "LREM %s_working 0 %s", lsraster_current_key, reply->str);
      if (!reply2) {
        lslogging_log_message("%s: LREM failed for key %s", id, lsraster_current_key);
        break;
      }      
      if (reply2->type == REDIS_REPLY_ERROR) {
        lslogging_log_message("%s: LREM failed for key %s: %s", id, lsraster_current_key, reply2->str);
        break;
      }
      
    } while(0);

    if (reply) {
      freeReplyObject(reply);
      reply = NULL;
    }

    if (reply2) {
      freeReplyObject(reply2);
      reply2 = NULL;
    }

    lsraster_working = 0;
    pthread_cond_signal(&lsraster_cond);
    pthread_mutex_unlock(&lsraster_mutex);
  }
}

pthread_t *lsraster_run() {
  pthread_create( &lsraster_thread, NULL, lsraster_worker, NULL);
  return &lsraster_thread;
}
