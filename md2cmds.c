/*! \file md2cmds.c
 *  \brief Implements commands to run the md2 diffractometer attached to a PMAC controled by postgresql
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */
#include "pgpmac.h"


pthread_cond_t  md2cmds_cond;		//!< condition to signal when it's time to run an md2 command
pthread_mutex_t md2cmds_mutex;		//!< mutex for the condition

int md2cmds_moving_queue_wait = 0;	//! wait for command to have been dequeued and run
pthread_cond_t  md2cmds_moving_cond;	//!< coordinate call and response
pthread_mutex_t md2cmds_moving_mutex;	//!< message passing between md2cmds and pg
pmac_cmd_queue_t *md2cmds_moving_pq;	//!< pmac queue item from last command

int md2cmds_moving_count = 0;

char md2cmds_cmd[MD2CMDS_CMD_LENGTH];	//!< our command;

lsredis_obj_t *md2cmds_md_status_code;

static pthread_t md2cmds_thread;

static int rotating = 0;		//!< flag: when omega is in position after a rotate we want to re-home omega

static double md2cmds_capz_moving_time = NAN;

/** set up moving motors in a coordinate system
 */

void md2cmds_move_prep( int mmask) {
  int flag;

  pthread_mutex_lock( &lspmac_moving_mutex);
  flag = (lspmac_moving_flags & mmask) != 0;
  pthread_mutex_unlock( &lspmac_moving_mutex);

  //
  // Only wait for the all clear if it's not all clear already
  //
  if( flag) {
    //
    // Clear the motion flags for the given coordinate system(s)
    // Then set them.
    // Each time we wait until we've read back
    // the changed values
    //
    // This guarantees that when we are waiting for motion to stop that it did, in fact, start
    //
    
    pthread_mutex_lock( &md2cmds_moving_mutex);
    md2cmds_moving_queue_wait = 1;
    pthread_mutex_unlock( &md2cmds_moving_mutex);
    
    //
    // Clear the centering and alignment stage flags
    //
    lspmac_SockSendDPline( "move_prep", "M5075=(M5075 | %d) ^ %d", mmask, mmask);
    
    pthread_mutex_lock( &md2cmds_moving_mutex);
    while( md2cmds_moving_queue_wait)
      pthread_cond_wait( &md2cmds_moving_cond, &md2cmds_moving_mutex);
    pthread_mutex_unlock( &md2cmds_moving_mutex);
    
    //
    // Make sure the command propagates back to the status
    //
    pthread_mutex_lock( &lspmac_moving_mutex);
    while( (lspmac_moving_flags & mmask) != 0)
      pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);

    lslogging_log_message( "md2cmds_move_prep: lspmac_moving_flags = %d", lspmac_moving_flags);
    pthread_mutex_unlock( &lspmac_moving_mutex);
  }


  //
  // set a flag so the event listener doesn't look at zero motion before we start and think we are done
  //
  pthread_mutex_lock( &md2cmds_moving_mutex);
  if( md2cmds_moving_count == 0)
    md2cmds_moving_count = -1;
  md2cmds_moving_queue_wait = 1;
  pthread_mutex_unlock( &md2cmds_moving_mutex);

  //
  // Now set the given motion flags
  //
  lspmac_SockSendDPline( "move_prep", "M5075=(M5075 | %d)", mmask);

  //
  // wait for the command to be sent
  //
  pthread_mutex_lock( &md2cmds_moving_mutex);
  while( md2cmds_moving_queue_wait)
    pthread_cond_wait( &md2cmds_moving_cond, &md2cmds_moving_mutex);
  pthread_mutex_unlock( &md2cmds_moving_mutex);

  //
  // Make sure it propagates
  //
  pthread_mutex_lock( &lspmac_moving_mutex);
  while( (lspmac_moving_flags & mmask) != mmask)
    pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);

  lslogging_log_message( "md2cmds_move_prep: lspmac_moving_flags = %d", lspmac_moving_flags);
  pthread_mutex_unlock( &lspmac_moving_mutex);
}

/** Wait for the movement to stop
 */
void md2cmds_move_wait( int mmask) {
  //
  // Just wait until the motion flags are lowered
  // Note this does not mean the motors are done moving,
  // just that the motion program is done.
  // 
  // Look for the "In Position" events to see if we are really done
  //
  // We are assuming that the "Moving" callbacks were received
  // before the motion programs have all finished.  Probably a reasonable
  // expectation but not really guaranteed
  //

  //
  // Dummy command that will get queued after our moves
  //
  pthread_mutex_lock( &md2cmds_moving_mutex);
  md2cmds_moving_queue_wait = 1;
  pthread_mutex_unlock( &md2cmds_moving_mutex);

  lspmac_SockSendDPline( "move_wait", "i65=4");

  pthread_mutex_lock( &md2cmds_moving_mutex);
  while( md2cmds_moving_queue_wait)
    pthread_cond_wait( &md2cmds_moving_cond, &md2cmds_moving_mutex);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
  

  //
  // Wait for the motion programs to finish
  //
  pthread_mutex_lock( &lspmac_moving_mutex);
  while( lspmac_moving_flags & mmask)
    pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);
  pthread_mutex_unlock( &lspmac_moving_mutex);

  //
  // Wait for the In Position events
  //
  pthread_mutex_lock( &md2cmds_moving_mutex);
  while( md2cmds_moving_count > 0)
    pthread_cond_wait( &md2cmds_moving_cond, &md2cmds_moving_mutex);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
}

double md2cmds_prep_axis( lspmac_motor_t *mp, double pos, int *there_already) {
  double rtn;
  double u2c;
  double precision;
  double current_pos;
  double neutral_pos;

  pthread_mutex_lock( &(mp->mutex));

  u2c         = lsredis_getd( mp->u2c);
  neutral_pos = lsredis_getd( mp->neutral_pos);
  precision   = lsredis_getd( mp->precision);
  current_pos = mp->position;

  if( fabs( pos - current_pos) < precision) {
    *there_already = 1;
    mp->motion_seen = 0;
    mp->not_done    = 1;
  } else {
    *there_already = 0;
    mp->motion_seen = 0;
    mp->not_done    = 1;
  }
  rtn = u2c   * (pos + neutral_pos);

  pthread_mutex_unlock( &(mp->mutex));

  return rtn;
}



void md2cmds_organs_move_presets( char *pay, char *paz, char *pcy, char *pcz, char *psz) {
  double ay,   az,  cy,  cz,  sz;
  int    cay, caz, ccy, ccz, csz;
  int    cay_there, caz_there, ccy_there, ccz_there, csz_there;
  int err;

  err = lsredis_find_preset( apery->name, pay, &ay);
  if( err == 0) {
    lslogging_log_message( "md2cmds_move_organs_presets: no preset '%s' for motor '%s'", pay, apery->name);
    return;
  }
  
  err = lsredis_find_preset( aperz->name, paz, &az);
  if( err == 0) {
    lslogging_log_message( "md2cmds_move_organs_presets: no preset '%s' for motor '%s'", paz, aperz->name);
    return;
  }
  
  err = lsredis_find_preset( capy->name, pcy, &cy);
  if( err == 0) {
    lslogging_log_message( "md2cmds_organs_move_presets: no preset '%s' for motor '%s'", pcy, capy->name);
    return;
  }

  err = lsredis_find_preset( capz->name, pcz, &cz);
  if( err == 0) {
    lslogging_log_message( "md2cmds_organs_move_presets: no preset '%s' for motor '%s'", pcz, capz->name);
    return;
  }

  err = lsredis_find_preset( scint->name, psz, &sz);
  if( err == 0) {
    lslogging_log_message( "md2cmds_organs_move_presets: no preset '%s' for motor '%s'", psz, scint->name);
    return;
  }

  cay = md2cmds_prep_axis( apery, ay, &cay_there);
  caz = md2cmds_prep_axis( aperz, az, &caz_there);
  ccy = md2cmds_prep_axis( capy,  cy, &ccy_there);
  ccz = md2cmds_prep_axis( capz,  cz, &ccz_there);
  csz = md2cmds_prep_axis( scint, sz, &csz_there);
  
  //
  // 170          LS-CAT Move U, V, W, X, Y, Z Absolute
  // 	       	      Q40     = X Value
  //		      Q41     = Y Value
  // 		      Q42     = Z Value
  //		      Q43     = U Value
  //		      Q44     = V Value
  //		      Q45     = W Value
  //
  
  if( cay_there==0 || caz_there==0 || ccy_there==0 || ccz_there==0 || csz_there==0)
    lspmac_SockSendDPline( "organs", "&5 Q40=0 Q41=%d Q42=%d Q43=%d Q44=%d Q45=%d Q100=16 B170R", cay, caz, ccy, ccz, csz);

}


/** Transfer a sample
 */
void md2cmds_transfer() {
  int nextsample, abort_now;
  double esttime;
  double ax, ay, az, cx, cy, horz, vert, oref;
  int err;
  int motion_mask;

  nextsample = lspg_nextsample_all( &err);
  if( err) {
    lslogging_log_message( "md2cmds_transfer: no sample requested to be transfered, false alarm");
    return;
  }
  
  //
  // BLUMax sets up an abort dialogbox here.  Probably we should figure out how we are going to handle that.
  //

  // Wait for everything to stop moving
  // TODO: timeout and abort if we are moving forever
  //
  md2cmds_move_wait( 0);

  pthread_mutex_lock( &md2cmds_moving_mutex);
  while( md2cmds_moving_count > 0)
    pthread_cond_wait( &md2cmds_moving_cond, &md2cmds_moving_mutex);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
  
  //
  // get positions we'll be needed to report to postgres
  //
  ax = lspmac_getPosition(alignx);
  ay = lspmac_getPosition(aligny);
  az = lspmac_getPosition(alignz);
  cx = lspmac_getPosition(cenx);
  cy = lspmac_getPosition(ceny);
  oref = lsredis_getd(lsredis_get_obj( "omega.reference")) * M_PI/180.;

  horz = cx * cos(oref) + cy * sin(oref);
  vert = cx * sin(oref) - cy * cos(oref);

  if( lsredis_getd( capz->u2c) <= 0.0 || lsredis_getd( capz->max_speed) <= 0.0 || lsredis_getd( capz->max_accel) <= 0.0) {
    esttime = 0.0;
  } else {
    
    // Here we assume moving the capilary is the rate limiting step in preparing the MD2.
    //
    // TODO: look at factors in which something besides the capilary determines the time.
    //
    // pretend we are going to zero instead of the "Out" position.  This should be less than a 5% error
    // and is probably not too horrible
    //
    // This also treats S curve acceleration as taking the same time as linear acceleration.
    //
    esttime  = lspmac_getPosition( capz)/lsredis_getd( capz->u2c)/(lsredis_getd( capz->max_speed));	// Time if we moved at constant velocity
    esttime += lsredis_getd( capz->max_speed)/lsredis_getd(capz->max_accel);				// Correction for time spent accelerating
    esttime /= 1000.;											// convert from milliseconds to seconds
  }

  lspg_starttransfer_call( nextsample, lspmac_getBIPosition( sample_detected), ax, ay, az, horz, vert, esttime);

  // put the light down if it's not already
  //
  if( lspmac_getBIPosition( blight_down) != 1)
    blight_ud->moveAbs( blight_ud, 0);
  
  // Pull the fluorescence detector out of the way
  //
  if( lspmac_getBIPosition( fluor_back) != 1)
    blight_ud->moveAbs( fluo, 0);
  
  //  get ready to move the organs, omega, kappa, and phi
  //          omega    organs    kappa/phi
  motion_mask = 1    |  16      | 64;
  md2cmds_move_prep( motion_mask);
  //
  // Put the organs into position
  //
  md2cmds_organs_move_presets( "In", "Cover", "In", "Cover", "Cover");

  //
  // Home Kappa
  //
  lspmac_home1_queue( kappa);

  //
  // Home omega
  //
  lspmac_home1_queue( omega);

  //
  // wait for kappa cause we can't home phi until kappa's done
  //
  lspmac_moveabs_wait( kappa);
  
  //
  // Home phi (whatever that means)
  //
  lspmac_home1_queue( phi);
  {
    pmac_cmd_queue_t *mypq;
    //
    // Do a little dance to have the md2cmds_moving routines see the
    // last move command we sent to the pmac
    //
    // try not to grab too many mutexs at the same time to lower the chance of a deadlock.
    //
    pthread_mutex_lock( &phi->mutex);
    mypq = phi->pq;
    pthread_mutex_unlock( &phi->mutex);
    
    pthread_mutex_lock( &md2cmds_moving_mutex);
    md2cmds_moving_pq = mypq;
    pthread_mutex_unlock( &md2cmds_moving_mutex);
  }

  // Now let's get back to postresql (remember our query so long ago?)
  //
  lspg_starttransfer_wait();

  //
  // It's possible that the sample that's mounted is unknown to the robot.
  // If so then we need to abort after we're done moving stuff
  //
  if( lspg_starttransfer.no_rows_returned || lspg_starttransfer.starttransfer != 1)
    abort_now = 1;
  else
    abort_now = 0;

  lspg_starttransfer_done();

  //
  // Wait for all those motors to stop moving
  //
  md2cmds_move_wait( motion_mask);

  // TODO: check that all the motors are where we told them to go  
  //

  if( abort_now) {
    lslogging_log_message( "md2cmds_transfer: Apparently there is a sample mounted already but we don't know where it is supposed to go");
    return;
  }
  
  // refuse to go on if we do not have positive confirmation that the backlight is down and the
  // fluorescence detector is back
  //
  if( lspmac_getBIPosition( blight_down) != 1 ||lspmac_getBIPosition( fluor_back) != 1) {
    lslogging_log_message( "md2cmds_transfer: It looks like either the back light is not down or the fluoescence dectector is not back");
    return;
  }

  //
  // Wait for the robot to unlock the cryo which signals us that we need to
  // move the cryo back and drop air rights
  //
  lspg_waitcryo_all();

  // Move the cryo back
  //
  cryo->moveAbs( cryo, 1);
  lspmac_moveabs_wait( cryo);

  // simplest query yet!
  lspg_query_push( lspg_waitcryo_cb, "SELECT px.dropairrights()");

  // wait for the result
  // TODO: find an easy way out of this in case of error
  //
  lspg_getcurrentsampleid_wait_for_id( nextsample);

  // grab the airrights again
  //
  lspg_demandairrights_all();
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
    lspmac_move_or_jog_abs_queue( phi,   0.0, 0);
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
    lspmac_move_or_jog_abs_queue( phi,  0.0, 0);
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
    lspmac_move_or_jog_abs_queue(    phi,   0.0, 0);
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
  int cx_there, cy_there, ax_there, ay_there, az_there;

  cx_cts = md2cmds_prep_axis( cenx,   cx, &cx_there);
  cy_cts = md2cmds_prep_axis( ceny,   cy, &cy_there);
  ax_cts = md2cmds_prep_axis( alignx, ax, &ax_there);
  ay_cts = md2cmds_prep_axis( aligny, ay, &ay_there);
  az_cts = md2cmds_prep_axis( alignz, az, &az_there);

  if( cx_there==0 || cy_there==0)
    lspmac_SockSendDPline( NULL, "&2 Q100=2 Q20=%.1f Q21=%.1f B150R", cx_cts, cy_cts);

  if( ax_there==0 || ay_there==0 || az_there==0)
    lspmac_SockSendDPline( "mvcenter_move", "&3 Q100=4 Q30=%.1f Q31=%.1f Q32=%.1f B160R", ax_cts, ay_cts, az_cts);
  
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
    //
    if( md2cmds_moving_count > 0)
      md2cmds_moving_count--;
  }

  lsredis_setstr( md2cmds_md_status_code, "%s", md2cmds_moving_count ? "4" : "3");
  
  if( md2cmds_moving_count == 0)
    pthread_cond_signal( &md2cmds_moving_cond);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
  
}



void md2cmds_organs_prep() {
  //
  // we are coordinate system 5,  mask is 1 << (cs - 1)
  //
  md2cmds_move_prep( 16);
}


void md2cmds_kappaphi_move( double kappa_deg, double phi_deg) {
  int kc, pc;
  int k_there, p_there;

  // coordinate system 7
  // 1 << (coord sys no - 1) = 64

  kc = md2cmds_prep_axis( kappa, kappa_deg, &k_there);
  pc = md2cmds_prep_axis( kappa, phi_deg,   &p_there);

  //  ;150		LS-CAT Move X, Y Absolute
  //  ;			Q20    = X Value
  //  ;			Q21    = Y Value
  //  ;			Q100   = 1 << (coord sys no  - 1)

  if( k_there==0 || p_there==0)
    lspmac_SockSendDPline( "kappaphi_move", "&7 Q20=%d Q21=%d Q100=64", kc, pc);

}


void md2cmds_organs_wait() {
  //
  // we are coordinate system 5,  mask is 1 << (cs - 1)
  //
  md2cmds_move_wait( 16);
}


/** Collect some data
 */
void md2cmds_collect() {
  long long skey;	//!< index of shot to be taken
  double p170;		//!< start cnts
  double p171;		//!< delta cnts
  double p173;		//!< omega velocity cnts/msec
  double p175;		//!< acceleration time (msec)
  double p180;		//!< exposure time (msec)
  int center_request;	//!< one of the stages, at least, needs to be moved
  double u2c;		//!< unit to counts conversion
  double neutral_pos;	//!< nominal zero offset
  double max_accel;	//!< maximum acceleration allowed for omega
  double kappa_pos;	//!< current kappa position in case we need to move phi only
  double phi_pos;	//!< current phi position in case we need to move kappa only
  int motion_mask;	//!< combined motion mask to set up waiting

  u2c         = lsredis_getd( omega->u2c);
  neutral_pos = lsredis_getd( omega->neutral_pos);
  max_accel   = lsredis_getd( omega->max_accel);

  //
  // Put the organs into position
  //
  motion_mask = 16;

  md2cmds_move_prep( motion_mask);
  md2cmds_organs_move_presets( "In", "In", "In", "In", "Cover");
  md2cmds_move_wait( motion_mask);

  //
  // reset shutter has opened flag
  //
  lspmac_SockSendDPline( NULL, "P3001=0 P3002=0");

  while( 1) {
    lspg_nextshot_call();

    motion_mask = 0;

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
	 //
	 // Don't move if we are within 0.1 microns of our destination
	 //
	 (fabs( lspg_nextshot.cx - cenx->position) > 0.1) ||
	 (fabs( lspg_nextshot.cy - ceny->position) > 0.1) ||
	 (fabs( lspg_nextshot.ax - alignx->position) > 0.1) ||
	 (fabs( lspg_nextshot.ay - aligny->position) > 0.1) ||
	 (fabs( lspg_nextshot.az - alignz->position) > 0.1)) {

	motion_mask |= 6;
	center_request = 1;
	md2cmds_move_prep( 6);
	md2cmds_mvcenter_move( lspg_nextshot.cx, lspg_nextshot.cy, lspg_nextshot.ax, lspg_nextshot.ay, lspg_nextshot.az);
      }
    }

    // Maybe move kappa and/or phi
    //
    if( !lspg_nextshot.dsphi_isnull || !lspg_nextshot.dskappa_isnull) {

      kappa_pos = lspg_nextshot.dskappa_isnull ? lspmac_getPosition( kappa) : lspg_nextshot.dskappa;
      phi_pos   = lspg_nextshot.dsphi_isnull   ? lspmac_getPosition( phi)   : lspg_nextshot.dsphi;

      motion_mask |= 64;
      md2cmds_move_prep( 64);
      md2cmds_kappaphi_move( kappa_pos, phi_pos);
    }

  
    if( motion_mask)
      md2cmds_move_wait( motion_mask);

    //
    // Calculate the parameters we'll need to run the scan
    //
    p180 = lspg_nextshot.dsexp * 1000.0;
    p170 = u2c * (lspg_nextshot.sstart + neutral_pos);
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
    lspmac_SockSendDPline( NULL, "&1 P170=%.1f P171=%.1f P173=%.1f P174=0 P175=%.1f P176=0 P177=1 P178=0 P180=%.1f M431=1 &1B131R",
			     p170,     p171,     p173,            p175,                          p180);



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


    //
    // Signal the detector to start reading out
    //
    lspg_query_push( NULL, "SELECT px.unlock_diffractometer()");

    //
    // Update the shot status
    //
    lspg_query_push( NULL, "SELECT px.shots_set_state(%lld, 'Writing')", skey);

    //
    // reset shutter has opened flag
    //
    lspmac_SockSendDPline( NULL, "P3001=0");

    //
    // Move the center/alignment stages to the next position
    //
    // TODO: position omega for the next shot.  During data collection the motion program
    // makes a good guess but for ortho snaps it is wrong.  We should add an argument to the motion program
    //

      
    if( !lspg_nextshot.active2_isnull && lspg_nextshot.active2) {
      if(
	 (fabs( lspg_nextshot.cx2 - cenx->position) > 0.1) ||
	 (fabs( lspg_nextshot.cy2 - ceny->position) > 0.1) ||
	 (fabs( lspg_nextshot.ax2 - alignx->position) > 0.1) ||
	 (fabs( lspg_nextshot.ay2 - aligny->position) > 0.1) ||
	 (fabs( lspg_nextshot.az2 - alignz->position) > 0.1)) {

	center_request = 1;
	md2cmds_move_prep( 6);
	md2cmds_mvcenter_move( lspg_nextshot.cx, lspg_nextshot.cy, lspg_nextshot.ax, lspg_nextshot.ay, lspg_nextshot.az);
	md2cmds_move_wait(6);
      }
    }
  }
}

/** Spin 360 and make a video (recenter first, maybe)
 *  
 */
void md2cmds_rotate() {
  double cx, cy, ax, ay, az;

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

    md2cmds_move_prep( 6);
    lslogging_log_message( "md2cmds_rotate: moving center");
    md2cmds_mvcenter_move( cx, cy, ax, ay, az);


    lslogging_log_message( "md2cmds_rotate: waiting for center move");
    md2cmds_move_wait(6);
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
}

/** Move centering and alignment tables as requested
 *  TODO: Implement
 */
void md2cmds_center() {
}

void md2cmds_move_prep_done_cb( char *event) {
  pthread_mutex_lock( &md2cmds_moving_mutex);
  md2cmds_moving_queue_wait = 0;
  pthread_cond_signal( &md2cmds_moving_cond);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
}

void md2cmds_move_wait_done_cb( char *event) {
  pthread_mutex_lock( &md2cmds_moving_mutex);
  md2cmds_moving_queue_wait = 0;
  pthread_cond_signal( &md2cmds_moving_cond);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
}

/** Time the capillary motion for the transfer routine
 */
void md2cmds_time_capz_cb( char *event) {
  static struct timespec capz_timestarted;	//!< track the time spent moving capz
  struct timespec now;
  int nsec, sec;

  if( strstr( event, "Moving") != NULL) {
    clock_gettime( CLOCK_REALTIME, &capz_timestarted);
  } else {
    clock_gettime( CLOCK_REALTIME, &now);

    sec  = now.tv_sec - capz_timestarted.tv_sec;
    nsec = 0;
    if( now.tv_nsec > capz_timestarted.tv_nsec) {
      sec--;
      nsec += 1000000000;
    }
    nsec += now.tv_nsec - capz_timestarted.tv_nsec;
    md2cmds_capz_moving_time = sec + nsec / 1000000000.;
  }
}

/*
 * queues an action for the md2cmds thread to work on
 *
 * \param timeout  wait this many seconds (double value, decimal secs OK), <0 means wait forever
 *
 * returns:
 *         non-zero on failure (see man page for pthread_mutex_timedlock for rtn value)
 *         0 on success
 */
int md2cmds_action_queue( double timeout, char *action) {
  int rtn;
  struct timespec waitforit;


  if( timeout < 0.0) {
    rtn = pthread_mutex_lock( &md2cmds_mutex);
  } else {
    clock_gettime( CLOCK_REALTIME, &waitforit);

    waitforit.tv_sec  += floor(timeout);
  
    waitforit.tv_nsec += (timeout - waitforit.tv_sec)*1.e9;
    while( waitforit.tv_nsec >= 1000000000) {
      waitforit.tv_sec++;
      waitforit.tv_nsec -= 1000000000;
    }

    rtn = pthread_mutex_timedlock( &md2cmds_mutex, &waitforit);
  }

  if( rtn == 0) {
    strncpy( md2cmds_cmd, action, MD2CMDS_CMD_LENGTH-1);
    md2cmds_cmd[MD2CMDS_CMD_LENGTH-1] = 0;
    pthread_cond_signal( &md2cmds_cond);
    pthread_mutex_unlock( &md2cmds_mutex);
  } else {
    if( rtn == ETIMEDOUT)
      lslogging_log_message( "md2cmds_action_queue: %s not queued, operation timed out", action);
    else
      lslogging_log_message( "md2cmds_action_queue: %s not queued with error code %d", action, rtn);
  }
  return rtn;
}

/** pause until md2cmds_worker has finished running the command
 */
void md2cmds_action_wait() {
  pthread_mutex_lock( &md2cmds_mutex);
  pthread_mutex_unlock( &md2cmds_mutex);
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

  md2cmds_md_status_code = lsredis_get_obj( "md2_status_code");
  lsredis_setstr( md2cmds_md_status_code, "7");
}

/** Start up the thread
 */
void md2cmds_run() {
  pthread_create( &md2cmds_thread, NULL,              md2cmds_worker, NULL);
  lsevents_add_listener( "omega crossed zero",        md2cmds_rotate_cb);
  lsevents_add_listener( ".+ (Moving|In Position)",   md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "capz (Moving|In Position)", md2cmds_time_capz_cb);
  lsevents_add_listener( "move_prep command done",    md2cmds_move_prep_done_cb);
  lsevents_add_listener( "move_wait command done",    md2cmds_move_wait_done_cb);
}
