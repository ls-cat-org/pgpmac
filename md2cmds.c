/*! \file md2cmds.c
 *  \brief Implements commands to run the md2 diffractometer attached to a PMAC controled by postgresql
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */
#include "pgpmac.h"


pthread_cond_t  md2cmds_cond;		//!< condition to signal when it's time to run an md2 command
pthread_mutex_t md2cmds_mutex;		//!< mutex for the condition

pthread_cond_t  md2cmds_moving_cond;	//!< coordinate call and response
pthread_mutex_t md2cmds_moving_mutex;	//!< message passing between md2cmds and pg
pmac_cmd_queue_t *md2cmds_moving_pq;	//!< pmac queue item from last command

int md2cmds_moving_count = 0;

char md2cmds_cmd[MD2CMDS_CMD_LENGTH];	//!< our command;

static pthread_t md2cmds_thread;

static int rotating = 0;		//!< flag: when omega is in position after a rotate we want to re-home omega


/** Transfer a sample
 *  TODO: Implement
 */
void md2cmds_transfer() {
}


/** Move a motor to the position requested
 */
void md2cmds_moveAbs(
		     const char *ccmd			/**< [in] The full command string to parse, ie, "moveAbs omega 180"	*/
		     ) {
  char *cmd;
  char *ignore;
  char *ptr;
  char *mtr;
  char *pos;
  double fpos;
  char *endptr;
  lspmac_motor_t *mp;
  int i;

  // ignore nothing
  if( ccmd == NULL || *ccmd == 0) {
    return;
  }

  // operate on a copy of the string since strtok_r will modify its argument
  //
  cmd = strdup( ccmd);

  // Parse the command string
  //
  ignore = strtok_r( cmd, " ", &ptr);
  if( ignore == NULL) {
    lslogging_log_message( "md2cmds_moveAbs: ignoring blank command '%s'", cmd);
    free( cmd);
    return;
  }

  // The first string should be "moveAbs" cause that's how we got here.
  // Toss it.
  
  mtr = strtok_r( NULL, " ", &ptr);
  if( mtr == NULL) {
    lslogging_log_message( "md2cmds moveAbs error: missing motor name");
    free( cmd);
    return;
  }

  mp = NULL;
  for( i=0; i<lspmac_nmotors; i++) {
    if( strcmp( lspmac_motors[i].name, mtr) == 0) {
      mp = &(lspmac_motors[i]);
      break;
    }
  }
  if( mp == NULL) {
    lslogging_log_message( "md2cmds moveAbs error: cannot find motor %s", mtr);
    free( cmd);
    return;
  }

  pos = strtok_r( NULL, " ", &ptr);
  if( pos == NULL) {
    lslogging_log_message( "md2cmds moveAbs error: missing position");
    free( cmd);
    return;
  }

  fpos = strtod( pos, &endptr);
  if( pos == endptr) {
    //
    // Maybe we have a preset.  Give it a whirl
    // In any case we are done here.
    //
    lspmac_move_preset_queue( mp, pos);
    free( cmd);
    return;
  }

  if( mp != NULL && mp->moveAbs != NULL) {
    wprintw( term_output, "Moving %s to %f\n", mtr, fpos);
    wnoutrefresh( term_output);
    mp->moveAbs( mp, fpos);
  }

  free( cmd);
}


/** Move md2 devices to a preconfigured state.
 *  EMBL calls these states "phases" and this language is partially retained here
 *
 *  \param ccmd The full text of the command that sent us here
 */
void md2cmds_phase_change( const char *ccmd) {
  char *cmd;
  char *ignore;
  char *ptr;
  char *mode;
  
  if( ccmd == NULL || *ccmd == 0)
    return;

  // use a copy as strtok_r modifies the string it is parsing
  //
  cmd = strdup( ccmd);

  ignore = strtok_r( cmd, " ", &ptr);
  if( ignore == NULL) {
    lslogging_log_message( "md2cmds_phase_change: ignoring empty command string (how did we let things get this far?");
    free( cmd);
    return;
  }

  //
  // ignore should point to "mode" cause that's how we got here.  Ignore it
  //
  mode = strtok_r( NULL, " ", &ptr);
  if( mode == NULL) {
    lslogging_log_message( "md2cmds_phase_change: no mode specified");
    free( cmd);
    return;
  }
  
  if( strcmp( mode, "manualMount") == 0) {
    lspmac_move_or_jog_preset_queue( kappa, "manualMount", 1);
    lspmac_move_or_jog_preset_queue( omega, "manualMount", 0);
    lspmac_move_or_jog_abs_queue(    phi,   "manualMount", 0);
    lspmac_move_or_jog_preset_queue( aperz, "Cover", 1);
    lspmac_move_or_jog_preset_queue( capz,  "Cover", 1);
    lspmac_move_or_jog_preset_queue( scint, "Cover", 1);
    md2cmds_moveAbs( "moveAbs backLight 0");
    md2cmds_moveAbs( "moveAbs backLight.intensity 0");
    md2cmds_moveAbs( "moveAbs cryo 1");
    md2cmds_moveAbs( "moveAbs fluo 0");
    md2cmds_moveAbs( "moveAbs cam.zoom 1");
  } else if( strcmp( mode, "robotMount") == 0) {
    lspmac_home1_queue( kappa);
    lspmac_home1_queue( omega);
    lspmac_move_or_jog_abs_queue(    phi,   "manualMount", 0);
    lspmac_move_or_jog_preset_queue( apery, "In", 1);
    lspmac_move_or_jog_preset_queue( aperz, "In", 1);
    lspmac_move_or_jog_preset_queue( capz,  "Cover", 1);
    lspmac_move_or_jog_preset_queue( scint, "Cover", 1);
    md2cmds_moveAbs( "moveAbs backLight 0");
    md2cmds_moveAbs( "moveAbs backLight.intensity 0");
    md2cmds_moveAbs( "moveAbs cryo 1");
    md2cmds_moveAbs( "moveAbs fluo 0");
    md2cmds_moveAbs( "moveAbs cam.zoom 1");
  } else if( strcmp( mode, "center") == 0) {
    md2cmds_moveAbs( "moveAbs kappa 0");
    md2cmds_moveAbs( "moveAbs omega 0");
    lspmac_move_or_jog_abs_queue(    phi,   "manualMount", 0);
    lspmac_move_or_jog_preset_queue( apery, "In", 1);
    lspmac_move_or_jog_preset_queue( aperz, "In", 1);
    lspmac_move_or_jog_preset_queue( capy,  "In", 1);
    lspmac_move_or_jog_preset_queue( capz,  "In", 1);
    lspmac_move_or_jog_preset_queue( scint, "Cover", 1);
    md2cmds_moveAbs( "moveAbs backLight 1");
    md2cmds_moveAbs( "moveAbs cam.zoom 1");
    md2cmds_moveAbs( "moveAbs cryo 0");
    md2cmds_moveAbs( "moveAbs fluo 0");
  } else if( strcmp( mode, "dataCollection") == 0) {
    lspmac_move_or_jog_preset_queue( apery, "In", 1);
    lspmac_move_or_jog_preset_queue( aperz, "In", 1);
    lspmac_move_or_jog_preset_queue( capy,  "In", 1);
    lspmac_move_or_jog_preset_queue( capz,  "In", 1);
    lspmac_move_or_jog_preset_queue( scint, "Cover", 1);
    md2cmds_moveAbs( "moveAbs backLight 0");
    md2cmds_moveAbs( "moveAbs backLight.intensity 0");
    md2cmds_moveAbs( "moveAbs cryo 0");
    md2cmds_moveAbs( "moveAbs fluo 0");
  } else if( strcmp( mode, "beamLocation") == 0) {
    md2cmds_moveAbs( "moveAbs kappa 0");
    md2cmds_moveAbs( "moveAbs omega 0");
    lspmac_move_or_jog_preset_queue( apery, "In", 1);
    lspmac_move_or_jog_preset_queue( aperz, "In", 1);
    lspmac_move_or_jog_preset_queue( capy,  "In", 1);
    lspmac_move_or_jog_preset_queue( capz,  "In", 1);
    lspmac_move_or_jog_preset_queue( scint, "Scintillator", 1);
    md2cmds_moveAbs( "moveAbs backLight 0");
    md2cmds_moveAbs( "moveAbs cam.zoom 1");
    md2cmds_moveAbs( "moveAbs cryo 0");
    md2cmds_moveAbs( "moveAbs fluo 0");
  } else if( strcmp( mode, "safe") == 0) {
    md2cmds_moveAbs( "moveAbs kappa 0");
    md2cmds_moveAbs( "moveAbs omega 0");
    lspmac_move_or_jog_preset_queue( apery, "In", 1);
    lspmac_move_or_jog_preset_queue( aperz, "Cover", 1);
    lspmac_move_or_jog_preset_queue( capy,  "In", 1);
    lspmac_move_or_jog_preset_queue( capz,  "Cover", 1);
    lspmac_move_or_jog_preset_queue( scint, "Cover", 1);
    md2cmds_moveAbs( "moveAbs backLight 0");
    md2cmds_moveAbs( "moveAbs cam.zoom 1");
    md2cmds_moveAbs( "moveAbs cryo 0");
    md2cmds_moveAbs( "moveAbs fluo 0");
  }

  
  free( cmd);
}


/** Sets up a centering table and alignment table move
 *  Ensures that when we issue the move command that
 *  we can detect that the move happened.
 */
void md2cmds_mvcenter_prep() {
  pmac_cmd_queue_t *pq;
  int flag;

  pthread_mutex_lock( &lspmac_moving_mutex);
  flag = (lspmac_moving_flags & 6) != 0;
  pthread_mutex_unlock( &lspmac_moving_mutex);

  //
  // Only wait for the all clear if it's not all clear already
  // Otherwise we may get confused
  //
  if( flag) {
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
    pq = lspmac_SockSendline( "M5075=(M5075 | 6) ^ 6");
    
    pthread_mutex_lock( &pmac_queue_mutex);
    //
    // wait for the command to be sent
    //
    while( pq->time_sent.tv_sec==0)
      pthread_cond_wait( &pmac_queue_cond, &pmac_queue_mutex);
    pthread_mutex_unlock( &pmac_queue_mutex);
    
    //
    // Make sure the command propagates back to the status
    //
    pthread_mutex_lock( &lspmac_moving_mutex);
    while( (lspmac_moving_flags & 6) != 0)
      pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);

    lslogging_log_message( "md2cmds_mvcenter_prep: lspmac_moving_flags = %d", lspmac_moving_flags);
    pthread_mutex_unlock( &lspmac_moving_mutex);
  }


  //
  // set a flag so the event listener doesn't send a callback too soon
  //
  pthread_mutex_lock( &md2cmds_moving_mutex);
  md2cmds_moving_count = -1;
  pthread_mutex_unlock( &md2cmds_moving_mutex);

  //
  // Now set the centering and alignment stage flags
  //
  pq = lspmac_SockSendline( "M5075=(M5075 | 6)");

  pthread_mutex_lock( &pmac_queue_mutex);
  //
  // wait for the command to be sent
  //
  while( pq->time_sent.tv_sec==0)
    pthread_cond_wait( &pmac_queue_cond, &pmac_queue_mutex);
  pthread_mutex_unlock( &pmac_queue_mutex);

  //
  // Make sure it propagates
  //
  pthread_mutex_lock( &lspmac_moving_mutex);
  while( (lspmac_moving_flags & 6) != 6)
    pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);

  lslogging_log_message( "md2cmds_mvcenter_prep: lspmac_moving_flags = %d", lspmac_moving_flags);
  pthread_mutex_unlock( &lspmac_moving_mutex);
}

double md2cmds_prep_motion( lspmac_motor_t *mp, double pos) {
  double rtn;
  double u2c;

  pthread_mutex_lock( &(mp->mutex));
  u2c = lsredis_getd( mp->u2c);

  rtn = u2c   * pos;
  mp->motion_seen = 0;
  mp->not_done    = 1;
  pthread_mutex_unlock( &(mp->mutex));

  return rtn;
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

  cx_cts = md2cmds_prep_motion( cenx, cx);
  cy_cts = md2cmds_prep_motion( ceny, cy);
  ax_cts = md2cmds_prep_motion( alignx, ax);
  ay_cts = md2cmds_prep_motion( aligny, ay);
  az_cts = md2cmds_prep_motion( alignz, az);

  lspmac_SockSendline( "&2 Q100=2 Q20=%.1f Q21=%.1f B150R", cx_cts, cy_cts);
  md2cmds_moving_pq = lspmac_SockSendline( "&3 Q100=4 Q30=%.1f Q31=%.1f Q32=%.1f B160R", ax_cts, ay_cts, az_cts);
  
}

/** Wait for the centering and alignment tables to stop moving
 */
void md2cmds_mvcenter_wait() {
  //
  // Just wait until the motion flags are lowered
  // Note this does not mean the motors are done moving,
  // just that the motion program is done.
  // 
  // Look for the "In Position" events to see if we are really done
  //
  // We are assuming that the "Moving" callback was received and acted on
  // before the motion programs have all finished.  Probably a reasonable
  // expectation but not really guaranteed
  //

  pthread_mutex_lock( &pmac_queue_mutex);
  //
  // wait for the command to be sent
  //
  while( md2cmds_moving_pq->time_sent.tv_sec==0)
    pthread_cond_wait( &pmac_queue_cond, &pmac_queue_mutex);
  pthread_mutex_unlock( &pmac_queue_mutex);


  pthread_mutex_lock( &lspmac_moving_mutex);
  while( lspmac_moving_flags & 6)
    pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);
  pthread_mutex_unlock( &lspmac_moving_mutex);

  pthread_mutex_lock( &md2cmds_moving_mutex);
  while( md2cmds_moving_count > 0)
    pthread_cond_wait( &md2cmds_moving_cond, &md2cmds_moving_mutex);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
}


/** Track how many motors are moving
 */
void md2cmds_maybe_done_moving_cb( char *event) {

  pthread_mutex_lock(   &md2cmds_moving_mutex);
  if( strstr( event, "Moving") != NULL) {
    //
    // -1 is a flag indicating we're expecting some action
    //
    if( md2cmds_moving_count == -1)
      md2cmds_moving_count = 1;
    else
      md2cmds_moving_count++;
  } else {
    //
    // Shouldn't need this but just in case a move was not finished before we're ready
    // this might take care of the problem
    //
    if( md2cmds_moving_count > 0)
      md2cmds_moving_count--;
  }

  if( md2cmds_moving_count == 0)
    pthread_cond_signal( &md2cmds_moving_cond);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
  
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
  int center_request;
  double u2c;
  double max_accel;


  u2c       = lsredis_getd( omega->u2c);
  max_accel = lsredis_getd( omega->max_accel);

  //
  // reset shutter has opened flag
  //
  lspmac_SockSendline( "P3001=0 P3002=0");


  while( 1) {
    lspg_nextshot_call();

    //
    // This is where we'd tell the md2 to move the organs into position
    //
    lspg_nextshot_wait();

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
    p170 = u2c * lspg_nextshot.sstart;
    //    p171 = u2c * ( lspg_nextshot.sstart + lspg_nextshot.dsowidth);
    p171 = u2c * lspg_nextshot.dsowidth;
    p173 = fabs(p180) < 1.e-4 ? 0.0 : u2c * lspg_nextshot.dsowidth / p180;
    p175 = p173/max_accel;


    //
    // free up access to nextshot
    //
    lspg_nextshot_done();

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



    //
    // wait for the shutter to open
    //
    pthread_mutex_lock( &lspmac_shutter_mutex);
    if( lspmac_shutter_has_opened == 0)
      pthread_cond_wait( &lspmac_shutter_cond, &lspmac_shutter_mutex);


    //
    // wait for the shutter to close
    //
    if( lspmac_shutter_state == 1)
      pthread_cond_wait( &lspmac_shutter_cond, &lspmac_shutter_mutex);
    pthread_mutex_unlock( &lspmac_shutter_mutex);


    lspg_query_push( NULL, "SELECT px.unlock_diffractometer()");

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
	lspmac_moveabs_wait( cenx);
	lspmac_moveabs_wait( ceny);
	lspmac_moveabs_wait( alignx);
	lspmac_moveabs_wait( aligny);
	lspmac_moveabs_wait( alignz);
      }
    }

  }
}

/** Spin 360 and make a video (recenter first, maybe)
 *  
 */
void md2cmds_rotate() {
  int v;		//!< velocity (cnts/msec) for omega
  double cx, cy, ax, ay, az;
  struct timespec snooze;

  //
  // BLUMax disables scintilator here.
  //

  //
  // get the new center information
  //
  lslogging_log_message( "md2cmds_rotate: calling getcenter");
  lspg_getcenter_call();

  lslogging_log_message( "md2cmds_rotate: wait for getcenter");
  lspg_getcenter_wait();


  lslogging_log_message( "md2cmds_rotate: moving backlight up");
  // put up the back light
  blight_ud->moveAbs( blight_ud, 1);

  if( lspg_getcenter.no_rows_returned) {
    //
    // Always specify zoom even if no other center information is found
    //
    zoom->moveAbs( zoom, 1);	// default zoom is 1
  } else {
    lslogging_log_message( "md2cmds_rotate: getcenter returned dcx %f, dcy %f, dax %f, day %f, daz %f, zoom %d",
			   lspg_getcenter.dcx, lspg_getcenter.dcy, lspg_getcenter.dax, lspg_getcenter.day, lspg_getcenter.daz,lspg_getcenter.zoom);

    if( lspg_getcenter.zoom_isnull == 0) {
      zoom->moveAbs( zoom, lspg_getcenter.zoom);
    } else {
      zoom->moveAbs( zoom, 1);
    }

    //
    // Grab the current positions and perhaps add the tad specified by getcenter
    //
    cx = lspmac_getPosition( cenx);
    cy = lspmac_getPosition( ceny);
    ax = lspmac_getPosition( alignx);
    ay = lspmac_getPosition( aligny);
    az = lspmac_getPosition( alignz);
    lslogging_log_message( "md2cmds_rotate: actual positions cx %f, cy %f, ax %f, ay %f, az %f", cx, cy, ax, ay, az);

    if( lspg_getcenter.dcx_isnull == 0)
      cx += lspg_getcenter.dcx;

    if( lspg_getcenter.dcy_isnull == 0)
      cy  += lspg_getcenter.dcy;
			  
    if( lspg_getcenter.dax_isnull == 0)
      ax  += lspg_getcenter.dax;

    if( lspg_getcenter.day_isnull == 0)
      ay  += lspg_getcenter.day;
			  
    if( lspg_getcenter.daz_isnull == 0)
      az  += lspg_getcenter.daz;
			  
    lslogging_log_message( "md2cmds_rotate: requested positions cx %f, cy %f, ax %f, ay %f, az %f", cx, cy, ax, ay, az);

    md2cmds_mvcenter_prep();
    lslogging_log_message( "md2cmds_rotate: moving center");
    md2cmds_mvcenter_move( cx, cy, ax, ay, az);


    lslogging_log_message( "md2cmds_rotate: waiting for center move");
    md2cmds_mvcenter_wait();
    lslogging_log_message( "md2cmds_rotate: done waiting");
  }
  lspg_getcenter_done();


  // Omega was just homed before we mounted the sample, don't do it again here
  
  // Report new center positions
  cx = lspmac_getPosition( cenx);
  cy = lspmac_getPosition( ceny);
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);
  lspg_query_push( NULL, "SELECT px.applycenter( %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f)", cx, cy, ax, ay, az, lspmac_getPosition(kappa), lspmac_getPosition( phi));

  lspmac_moveabs_wait( zoom);

  lslogging_log_message( "md2cmds_rotate: done with applycenter");
  lspmac_video_rotate( 4.0);
  lslogging_log_message( "md2cmds_rotate: starting rotation");
  rotating = 1;
}

/** Tell the database about the time we went through omega=zero.
 *  This should trigger the video feed server to starting making a movie.
 */
void md2cmds_rotate_cb( char *event) {
  struct tm t;
  int usecs;

  localtime_r( &(omega_zero_time.tv_sec), &t);
  
  lslogging_log_message( "md2cmds_rotate_cb: Here I am");

  usecs = omega_zero_time.tv_nsec / 1000;
  lspg_query_push( NULL, "SELECT px.trigcam('%d-%d-%d %d:%d:%d.%06d', %d, 0.0, 90.0)",
		   t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, usecs,
		   (int)(lspmac_getPosition( zoom)));

}

/** Now that we are done with the 360 rotation lets rehome right quick
 */
void md2cmds_maybe_rotate_done_cb( char *event) {
  if( rotating) {
    rotating = 0;
    lspmac_home1_queue( omega);
  }
}


/** Fix up xscale and yscale when zoom changes
 */
void md2cmds_set_scale_cb( char *event) {
  int mag;
  lsredis_obj_t *p1, *p2;
  char fmt;
  char *vp;

  mag = lspmac_getPosition( zoom);
  

  p1  = lsredis_get_obj( "cam.xScale");
  p2  = lsredis_get_obj( "cam.zoom.%d.ScaleX", mag);

  vp = lsredis_getstr( p2);
  lsredis_setstr( p2, vp);
  free( vp);

  p1  = lsredis_get_obj( "cam.yScale");
  p2  = lsredis_get_obj( "cam.zoom.%d.ScaleY", mag);

  vp = lsredis_getstr( p2);
  lsredis_setstr( p2, vp);
  free( vp);

  lspg_query_push( NULL, "SELECT pmac.md2_set_scales( %d)", mag);
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
    } else if( strncmp( md2cmds_cmd, "changeMode", 10) == 0) {
      md2cmds_phase_change( md2cmds_cmd);
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

  pthread_mutex_init( &md2cmds_moving_mutex, NULL);
  pthread_cond_init(  &md2cmds_moving_cond, NULL);


}

/** Start up the thread
 */
void md2cmds_run() {
  pthread_create( &md2cmds_thread, NULL,            md2cmds_worker, NULL);
  lsevents_add_listener( "omega crossed zero",      md2cmds_rotate_cb);
  lsevents_add_listener( "omega In Position",       md2cmds_maybe_rotate_done_cb);
  lsevents_add_listener( "align.x In Position",     md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "align.y In Position",     md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "align.z In Position",     md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "centering.x In Position", md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "centering.y In Position", md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "align.x Moving",          md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "align.y Moving",          md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "align.z Moving",          md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "centering.x Moving",      md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "centering.y Moving",      md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "cam.zoom In Position",    md2cmds_set_scale_cb);
}
