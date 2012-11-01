/*! \file md2cmds.c
 *  \brief Implements commands to run the md2 diffractometer attached to a PMAC controled by postgresql
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */
#include "pgpmac.h"


pthread_cond_t  md2cmds_cond;		//!< condition to signal when it's time to run an md2 command
pthread_mutex_t md2cmds_mutex;		//!< mutex for the condition

pthread_cond_t  md2cmds_pg_cond;	//!< coordinate call and response
pthread_mutex_t md2cmds_pg_mutex;	//!< message passing between md2cmds and pg

char md2cmds_cmd[MD2CMDS_CMD_LENGTH];	//!< our command;

static pthread_t md2cmds_thread;


/** Transfer a sample
 *  TODO: Implement
 */
void md2cmds_transfer() {
}

/** Return a time string for loggin
 *  Time is from the first call to this funciton.
 */
char *logtime() {
  static char rtn[128];
  static char tmp[64];
  static int first_time = 1;
  static struct timeval base;
  struct timeval now;
  struct tm nows;
  double diffs;

  if( first_time) {
    first_time=0;
    gettimeofday( &base, NULL);
    strftime(tmp, sizeof(tmp)-1, "%Y-%m-%d %H:%M:%S", localtime( &(base.tv_sec)));
    tmp[sizeof(tmp)-1]=0;
    snprintf( rtn, sizeof(rtn)-1, "%s.%06d", tmp, base.tv_usec);
    rtn[sizeof(rtn)-1]=0;
  } else {
    gettimeofday( &now, NULL);
    diffs =  (now.tv_sec - base.tv_sec);
    diffs += (now.tv_usec - base.tv_usec)/1000000.;
    snprintf( rtn, sizeof( rtn)-1, "%0.6f", diffs);
    rtn[sizeof(rtn)-1]=0;
  }

  return rtn;
}

/** Move a motor to the position requested
 */
void md2cmds_moveAbs(
		     char *cmd			/**< [in] The full command string to parse, ie, "moveAbs omega 180"	*/
		     ) {
  char *ignore;
  char *ptr;
  char *mtr;
  char *pos;
  double fpos;
  char *endptr;
  lspmac_motor_t *mp;
  int i;

  // Parse the command string
  //
  ignore = strtok_r( cmd, " ", &ptr);
  if( ignore == NULL) {
    //
    // Should generate error message
    // about blank command
    //
    return;
  }

  // The first string should be "moveAbs" cause that's how we got here.
  // Toss it.
  
  mtr = strtok_r( NULL, " ", &ptr);
  if( mtr == NULL) {
    //
    // Should generate error message
    // about missing motor name
    //
    return;
  }

  pos = strtok_r( NULL, " ", &ptr);
  if( pos == NULL) {
    //
    // Should generate error message
    // about missing position
    //
    return;
  }

  fpos = strtod( pos, &endptr);
  if( pos == endptr) {
    //
    // Should generate error message 
    // about bad double conversion
    //
    return;
  }
  
  mp = NULL;
  for( i=0; i<lspmac_nmotors; i++) {
    if( strcmp( lspmac_motors[i].name, mtr) == 0) {
      mp = &(lspmac_motors[i]);
      break;
    }
  }


  if( mp != NULL && mp->moveAbs != NULL) {
    wprintw( term_output, "Moving %s to %f\n", mtr, fpos);
    wnoutrefresh( term_output);
    mp->moveAbs( mp, fpos);
  }

}


/** Sets up a centering table and alignment table move
 *  Ensures that when we issue the move command that
 *  we can detect that the move happened.
 */
void md2cmds_mvcenter_prep() {
  //
  // Clears the motion flags for coordinate systems 2 and 3
  // Then sets them.
  // Each time we wait until we've read back
  // the changed values
  //
  // This guarantees that when we are waiting for motion to stop that it did, in fact, start
  //

  //
  // Clear the centering and alignment stage flags
  //
  lspmac_SockSendline( "M7075=(M7075 | 6) ^ 6");

  //
  // Make sure it propagates
  //
  pthread_mutex_lock( &lspmac_moving_mutex);
  while( lspmac_moving_flags & 6)
    pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);
  pthread_mutex_unlock( &lspmac_moving_mutex);

  //
  // Set the centering and alignment stage flags
  //
  lspmac_SockSendline( "M7075=(M7075 | 6)");

  //
  // Make sure it propagates
  //
  pthread_mutex_lock( &lspmac_moving_mutex);
  while( (lspmac_moving_flags & 6) == 0)
    pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);
  pthread_mutex_unlock( &lspmac_moving_mutex);
}

/** Move the centering and alignment tables
 */
void md2cmds_mvcenter_move(
			   double cx,	/**< [in] Requested Centering Table X		*/
			   double cy,	/**< [in] Requested Centering Table Y		*/
			   double ax,	/**< [in] Requested Alignment Table X		*/
			   double ay,	/**< [in] Requested Alignment Table Y		*/
			   double az	/**< [in] Requested Alignment Table Z		*/
			   ) {
  //
  // centering stage is coordinate system 2
  // alignment stage is coordinate system 3
  //
  
  double cx_cts, cy_cts, ax_cts, ay_cts, az_cts;

  cx_cts = cenx->u2c   * cx;
  cy_cts = ceny->u2c   * cy;
  ax_cts = alignx->u2c * ax;
  ay_cts = aligny->u2c * ay;
  az_cts = alignz->u2c * az;

  lspmac_SockSendline( "M7075=(M7075 | 2) &2 Q100=2 Q20=%.1f Q21=%.1f B150R", cx_cts, cy_cts);
  lspmac_SockSendline( "M7075=(M7075 | 4) &3 Q100=4 Q30=%.1f Q31=%.1f Q32=%.1f B160R", ax_cts, ay_cts, az_cts);
  
}

/** Wait for the centering and alignment tables to stop moving
 */
void md2cmds_mvcenter_wait() {
  //
  // Just wait until the motion flags are lowered
  //

  pthread_mutex_lock( &lspmac_moving_mutex);
  while( lspmac_moving_flags & 6)
    pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);
  pthread_mutex_unlock( &lspmac_moving_mutex);
}


/** Collect some data
 */
void md2cmds_collect() {
  long long skey;
  double p170;	// start cnts
  double p171;	// end cnts
  double p173;	// omega velocity cnts/msec
  double p175;	// acceleration time (msec)
  double p180;	// exposure time (msec)
  FILE *zzlog;
  struct timeval tt_base, tt_now;
  int center_request;

  zzlog = fopen( "/tmp/collect_log.txt", "w");
  fprintf( zzlog, "%s: Start md2cmds\n", logtime());
  fflush( zzlog);

  //
  // reset shutter has opened flag
  //
  lspmac_SockSendline( "P3001=0 P3002=0");


  while( 1) {
    fprintf( zzlog, "%s: call lspg_nextshot_call\n", logtime());
    fflush( zzlog);
    lspg_nextshot_call();

    //
    // This is where we'd tell the md2 to move the organs into position
    //

    fprintf( zzlog, "%s: call lspg_nextshot_wait\n", logtime());
    fflush( zzlog);

    lspg_nextshot_wait();
    fprintf( zzlog, "%s: returned from  lspg_nextshot_wait\n", logtime());
    fflush( zzlog);

    if( lspg_nextshot.no_rows_returned) {
      lspg_nextshot_done();
      break;
    }

    skey = lspg_nextshot.skey;
    lspg_query_push( NULL, "SELECT px.shots_set_state(%lld, 'Preparing')", skey);

    center_request = 0;
    if( lspg_nextshot.active) {
      if(
	 (fabs( lspg_nextshot.cx - cenx->position) > 0.1) ||
	 (fabs( lspg_nextshot.cy - ceny->position) > 0.1) ||
	 (fabs( lspg_nextshot.ax - alignx->position) > 0.1) ||
	 (fabs( lspg_nextshot.ay - aligny->position) > 0.1) ||
	 (fabs( lspg_nextshot.az - alignz->position) > 0.1)) {

	center_request = 1;
	md2cmds_mvcenter_prep();
	md2cmds_mvcenter_move( lspg_nextshot.cx, lspg_nextshot.cy, lspg_nextshot.ax, lspg_nextshot.ay, lspg_nextshot.az);
      }
    }

    if( !lspg_nextshot.dsphi_isnull) {
      lspmac_moveabs_queue( phi, lspg_nextshot.dsphi);
    }
  
    if( !lspg_nextshot.dskappa_isnull) {
      lspmac_moveabs_queue( kappa, lspg_nextshot.dskappa);
    }

  
    //
    // Wait for all those motors to stop
    //
    if( center_request) {
      md2cmds_mvcenter_wait();
    }

    if( !lspg_nextshot.dsphi_isnull) {
      lspmac_moveabs_wait( phi);
    }
  
    if( !lspg_nextshot.dskappa_isnull) {
      lspmac_moveabs_wait( kappa);
    }

    //
    // Calculate the parameters we'll need to run the scan
    //
    p180 = lspg_nextshot.dsexp * 1000.0;
    p170 = omega->u2c * lspg_nextshot.sstart;
    //    p171 = omega->u2c * ( lspg_nextshot.sstart + lspg_nextshot.dsowidth);
    p171 = omega->u2c * lspg_nextshot.dsowidth;
    p173 = fabs(p180) < 1.e-4 ? 0.0 : omega->u2c * lspg_nextshot.dsowidth / p180;
    p175 = p173/omega->max_accel;


    //
    // free up access to nextshot
    //
    lspg_nextshot_done();

    fprintf( zzlog, "%s: finished with lspg_nextshot_done, calling lspg_seq_run_prep_all\n", logtime());
    fflush( zzlog);

    //
    // prepare the database and detector to expose
    // On exit we own the diffractometer lock and
    // have checked that all is OK with the detector
    //
    lspg_seq_run_prep_all( skey,
			   kappa->position,
			   phi->position,
			   cenx->position,
			   ceny->position,
			   alignx->position,
			   aligny->position,
			   alignz->position
			   );

    
    fprintf( zzlog, "%s: finished with lspg_seq_run_prep_all\n", logtime());
    fflush( zzlog);
    //
    // make sure our has opened flag is down
    // wait for the p3001=0 command to be noticed
    //
    pthread_mutex_lock( &lspmac_shutter_mutex);
    if( lspmac_shutter_has_opened == 1)
      pthread_cond_wait( &lspmac_shutter_cond, &lspmac_shutter_mutex);
    pthread_mutex_unlock( &lspmac_shutter_mutex);

    //
    // Start the exposure
    //
    lspmac_SockSendline( "P170=%.1f P171=%.1f P173=%.1f P174=0 P175=%.1f P176=0 P177=1 P178=0 P180=%.1f M431=1 &1B131R",
			 p170,      p171,     p173,            p175,                          p180);


    fprintf( zzlog, "%s: sent command to pmac\n", logtime());
    fflush( zzlog);

    //
    // wait for the shutter to open
    //
    pthread_mutex_lock( &lspmac_shutter_mutex);
    if( lspmac_shutter_has_opened == 0)
      pthread_cond_wait( &lspmac_shutter_cond, &lspmac_shutter_mutex);

    fprintf( zzlog, "%s: shutter has opened\n", logtime());
    fflush( zzlog);

    //
    // wait for the shutter to close
    //
    if( lspmac_shutter_state == 1)
      pthread_cond_wait( &lspmac_shutter_cond, &lspmac_shutter_mutex);
    pthread_mutex_unlock( &lspmac_shutter_mutex);

    fprintf( zzlog, "%s: shutter now closed, unlocking diffractometer\n", logtime());
    fflush( zzlog);


    lspg_query_push( NULL, "SELECT px.unlock_diffractometer()");

    fprintf( zzlog, "%s: unlocked diffractometer\n", logtime());
    fflush( zzlog);

    lspg_query_push( NULL, "SELECT px.shots_set_state(%lld, 'Writing')", skey);

    //
    // reset shutter has opened flag
    //
    lspmac_SockSendline( "P3001=0");
    //
    // TODO:
    // wait for omega to stop moving then position it for the next frame
    //


    if( !lspg_nextshot.active2_isnull && lspg_nextshot.active2) {
      if(
	 (fabs( lspg_nextshot.cx2 - cenx->position) > 0.1) ||
	 (fabs( lspg_nextshot.cy2 - ceny->position) > 0.1) ||
	 (fabs( lspg_nextshot.ax2 - alignx->position) > 0.1) ||
	 (fabs( lspg_nextshot.ay2 - aligny->position) > 0.1) ||
	 (fabs( lspg_nextshot.az2 - alignz->position) > 0.1)) {

	center_request = 1;
	md2cmds_mvcenter_prep();
	md2cmds_mvcenter_move( lspg_nextshot.cx, lspg_nextshot.cy, lspg_nextshot.ax, lspg_nextshot.ay, lspg_nextshot.az);
	md2cmds_mvcenter_wait();
      }
    }

  }
  fprintf( zzlog, "%s: done\n", logtime());
  fflush( zzlog);
  fclose( zzlog);
}

/** Spin 360 and make a video
 *  TODO: Implement
 */
void md2cmds_rotate() {
}

/** Move centering and alignment tables as requested
 *  TODO: Implement
 */
void md2cmds_center() {
}



/** Our worker thread
 */
void *md2cmds_worker(
		     void *dummy		/**> [in] Unused but required by protocol		*/
		     ) {

  pthread_mutex_lock( &md2cmds_mutex);

  while( 1) {
    //
    // wait for someone to give us a command (and tell us they did so)
    //
    while( md2cmds_cmd[0] == 0)
      pthread_cond_wait( &md2cmds_cond, &md2cmds_mutex);

    if( strcmp( md2cmds_cmd, "transfer") == 0) {
      md2cmds_transfer();
    } else if( strcmp( md2cmds_cmd, "collect") == 0) {
      md2cmds_collect();
    } else if( strcmp( md2cmds_cmd, "rotate") == 0) {
      md2cmds_rotate();
    } else if( strcmp( md2cmds_cmd, "center") == 0) {
      md2cmds_center();
    } else if( strncmp( md2cmds_cmd, "moveAbs", 7) == 0) {
      md2cmds_moveAbs( md2cmds_cmd);
    }

    md2cmds_cmd[0] = 0;
  }
}


/** Initialize the md2cmds module
 */
void md2cmds_init() {
  memset( md2cmds_cmd, 0, sizeof( md2cmds_cmd));

  pthread_mutex_init( &md2cmds_mutex, NULL);
  pthread_cond_init( &md2cmds_cond, NULL);

  pthread_mutex_init( &md2cmds_pg_mutex, NULL);
  pthread_cond_init( &md2cmds_pg_cond, NULL);

}

/** Start up the thread
 */
void md2cmds_run() {
  pthread_create( &md2cmds_thread, NULL, md2cmds_worker, NULL);
}
