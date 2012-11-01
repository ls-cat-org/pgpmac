/*! \file lsupdate.c
 *  \brief Brings this MD2 code and the database kvs table into agreement
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */

#include "pgpmac.h"

static pthread_t lsupdate_thread;		//!< our worker thread


/** Query the motors and perhaps tell the DB about it
 */
void lsupdate_updateit(
		       int first_time		/**< [in] Flag: 1 means update everything, 0 means only send stuff that has changed  */
		       ) {
  static char s[4096];
  static char s1[512];
  lspmac_motor_t *mp;
  int i;
  int needComma;
  int gotone;

  needComma = 0;
  gotone = 0;
  s[0] = 0;
  strcpy(s, "select px.kvupdate('{");

  for( i=0; i<lspmac_nmotors; i++) {
    mp = &(lspmac_motors[i]);

    pthread_mutex_lock( &(mp->mutex));
    if( fabs( mp->position - mp->reported_position) < mp->update_resolution && first_time == 0) {
      pthread_mutex_unlock( &(mp->mutex));
    } else {

      gotone = 1;
      s1[0]=0;

      snprintf( s1, sizeof(s1)-1, mp->format, mp->position, sizeof( s1)-1);
      s1[sizeof(s1)-1] = 0;
    
      mp->reported_position = mp->position;
      pthread_mutex_unlock( &(mp->mutex));

      if( strlen(s1) + strlen(s) + 8 >= sizeof( s)-1) {
	// send off update now and reset s
	strcat( s, "}')");
	lspg_query_push( NULL, s);
	
	s[0] = 0;
	strcpy( s, "select px.kvupdate('{");
	needComma = 0;
      }

      if( needComma)
	strcat( s, ",");
      else
	needComma=1;

      strcat( s, "\"");
      strcat( s, s1);
      strcat( s, "\"");
    }
  }

  if( gotone) {
    strcat( s, "}')");
    lspg_query_push( NULL, s);
  }
}

/** Our worker thread
 */
void *lsupdate_worker(
		      void *dummy		/**< [in] Unused argument required by protocol		*/
		      ) {    
  sleep(10);
  lsupdate_updateit( 1);
  while( 1) {
    usleep( 500000);
    lsupdate_updateit( 0);
  }    
}

/** Initialize this module
 */
void lsupdate_init() {
}

/** run the update routines
 */
void lsupdate_run() {
  //  pthread_create( &lsupdate_thread, NULL, lsupdate_worker, NULL);
}
