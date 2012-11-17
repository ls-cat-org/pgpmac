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
void lsupdate_updateit() {
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
    //
    // Bit 0 of lspg_initialized is 0 if we've not yet initialized the motor values via the DB
    // Bit 1 of lspg_initialized is 0 if we've not yet sent any update for this motor
    //
    // Never update if the database has not initialized the motor values
    // Then, always update if we've not done so yet
    // Then, only update if the current position has changed significantly
    //
    if( ((mp->lspg_initialized & 1) == 0) ||
	((mp->lspg_initialized & 2) != 0) &&
	(fabs( mp->position - mp->reported_position) < mp->update_resolution)
	) {
      pthread_mutex_unlock( &(mp->mutex));
    } else {

      gotone = 1;
      s1[0]=0;

      snprintf( s1, sizeof(s1)-1, mp->update_format, mp->position);
      s1[sizeof(s1)-1] = 0;
    
      mp->reported_position = mp->position;
      mp->lspg_initialized |= 2;
      pthread_mutex_unlock( &(mp->mutex));

      if( strlen(s1) + strlen(s) + 32 >= sizeof( s)-1) {
	// send off update now and reset s
	strcat( s, "}'::text[])");
	lspg_query_push( NULL, s);
	
	s[0] = 0;
	strcpy( s, "select px.kvupdate('{");
	needComma = 0;
	gotone    = 0;
      }

      if( needComma)
	strcat( s, ",");
      else
	needComma=1;

      strcat( s, s1);
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
  static struct timespec naptime;

  naptime.tv_sec  = 0;
  naptime.tv_nsec = 500000000;
  while( 1) {
    lsupdate_updateit();
    nanosleep( &naptime, NULL);
  }    
}

/** Initialize this module
 */
void lsupdate_init() {
}

/** run the update routines
 */
void lsupdate_run() {
  pthread_create( &lsupdate_thread, NULL, lsupdate_worker, NULL);
}
