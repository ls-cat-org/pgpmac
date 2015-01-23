/*! \file md2cmds.c
 *  \brief Implements commands to run the md2 diffractometer attached to a PMAC controled by postgresql
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */
#include "pgpmac.h"

/*
The following 3 commands are acted on immediately without waiting for the "enter" key:
<pre>
 Command      Meaning
 pmac         change to PMAC mode (Real or Wannabe Experts only)
 md2cmds      change to MD-2 command mode
 quit         clean up and exit the program
</pre>

For the PMAC commands you'll need a manual or two.

Here are the MD2 commands.  Anything MD2 command that you can use from the remote interface can also be typed here.

All positions are in millimeters or degrees.

<pre>
 Command                                    Meaning

 abort                                      Stop the current motion and put the system into a known state

 changeMode  <mode>                         Where <mode> is one of "manualMount", "robotMount", "center", "dataCollection", "beamLocation", "safe"

 collect                                    Start collecting data

 moveAbs  <motor> <position_or_presetName>  Move the given motor to the said position.  Common preset names are "In", "Out", "Cover".

 moveRel  <motor> <relative_position>       Move the given motor by the relative amount from its current position

 nonrotate                                  Used for local centering when we do not want to trigger movie making

 rotate                                     Used for remote centering where we do want to make a movie

 run <motor> <command>                      Run a special command on <motor> where <command> is one of "home", "spin", "stop"

 set <motor> <preset>                       Set <motor>'s current position as <preset>.  <preset> will be created if it does not currently exist.

 settransferpoint                           Set the current motor positions at the alignment point for robot transfers.

 setbackvector                              Set the current alignment stage position as the Back preset and the difference between Back and Beam as Back_Vector

 test                                       Run unit tests (which are not very complete at this point)

 transfer                                   Transfer the next transfer

</pre>

Here are the motors:

<pre>

 Motor                       Presets

align.x                      Beam Back Back_Vector
align.y                      Beam Back Back_Vector
align.z                      Beam Back Back_Vector
appy                         In
appz                         In Out Cover
backLight                    On Off
backLight.factor
backLight.intensity
cam.zoom
capy                         In
capz                         In Out Cover
centering.x                  Beam
centering.y                  Beam
cryo
dryer
fluo
frontLight                   On Off
frontLight.factor
frontLight.intensity
kappa                        manualMount
lightPolar
omega                        manualMount Reference
phi
scint                        Photodiode Scintillator Cover
scint.focus                  tuner
smartMagnet

</pre>

All presets listed above are absolute positions EXCEPT Back_Vector.
For the alignment stage this is the amount to add to the Beam preset
to get to the Back preset.  Back_Vector is updated any time the
alignment stage is moved while in "center" or "dataCollection" mode.


*/


pthread_cond_t  md2cmds_cond;		//!< condition to signal when it's time to run an md2 command
pthread_mutex_t md2cmds_mutex;		//!< mutex for the condition

int md2cmds_moving_queue_wait = 0;	//! wait for command to have been dequeued and run
pthread_cond_t  md2cmds_moving_cond;	//!< coordinate call and response
pthread_mutex_t md2cmds_moving_mutex;	//!< message passing between md2cmds and pg

int md2cmds_homing_count = 0;		//!< We've asked a motor to home
pthread_cond_t md2cmds_homing_cond;	//!< coordinate homing and homed
pthread_mutex_t md2cmds_homing_mutex;	//!< our mutex;


int md2cmds_moving_count = 0;

char md2cmds_cmd[MD2CMDS_CMD_LENGTH];	//!< our command;

lsredis_obj_t *md2cmds_md_status_code;

static pthread_t md2cmds_thread;

static int rotating = 0;		//!< flag: when omega is in position after a rotate we want to re-home omega

static int abort_requested = 0;		//!< flag: indicates an out of band abort request
static pthread_mutex_t abort_requested_mutex;

static double md2cmds_capz_moving_time = NAN;

static struct hsearch_data md2cmds_hmap;

static regex_t md2cmds_cmd_regex;


typedef struct md2cmds_cmd_kv_struct {
  char *k;
  int (*v)( const char *);
} md2cmds_cmd_kv_t;

int md2cmds_abort(            const char *);
int md2cmds_collect(          const char *);
int md2cmds_moveAbs(          const char *);
int md2cmds_moveRel(          const char *);
int md2cmds_phase_change(     const char *);
int md2cmds_run_cmd(          const char *);
int md2cmds_rotate(           const char *);
int md2cmds_nonrotate(        const char *);
int md2cmds_set(              const char *);
int md2cmds_settransferpoint( const char *);
int md2cmds_setbackvector(    const char *);
int md2cmds_test(             const char *);
int md2cmds_transfer(         const char *);

//
// Commands that do not rely on lspg calls
//
static md2cmds_cmd_kv_t md2cmds_cmd_kvs[] = {
  { "abort",            md2cmds_abort},
  { "changeMode",       md2cmds_phase_change},
  { "moveAbs",          md2cmds_moveAbs},
  { "moveRel",          md2cmds_moveRel},
  { "run",              md2cmds_run_cmd},
  { "test",             md2cmds_test},
  { "set",              md2cmds_set},
  { "setbackvector",    md2cmds_setbackvector}
};

//
// Commands that do rely on lspg calls
//
static md2cmds_cmd_kv_t md2cmds_cmd_pg_kvs[] = {
  { "collect",          md2cmds_collect},
  { "nonrotate",        md2cmds_nonrotate},
  { "rotate",           md2cmds_rotate},
  { "settransferpoint", md2cmds_settransferpoint},
  { "transfer",         md2cmds_transfer}
};

void md2cmds_push_queue( char *action) {

  if( pthread_mutex_trylock( &md2cmds_mutex) == 0) {
    strncpy( md2cmds_cmd, action, MD2CMDS_CMD_LENGTH-1);
    md2cmds_cmd[MD2CMDS_CMD_LENGTH-1] = 0;
    pthread_cond_signal( &md2cmds_cond);
    pthread_mutex_unlock( &md2cmds_mutex);
  } else {
    lslogging_log_message( "md2cmds_push_queue: MD2 command '%s' ignored.  Already running '%s'", action, md2cmds_cmd);
  }
}

void md2cmds_home_prep() {
  pthread_mutex_lock( &md2cmds_homing_mutex);
  md2cmds_homing_count = -1;
  pthread_mutex_unlock( &md2cmds_homing_mutex);
}


int md2cmds_home_wait( double timeout_secs) {
  struct timespec timeout, now;
  double isecs, fsecs;
  int err;

  clock_gettime( CLOCK_REALTIME, &now);

  fsecs = modf( timeout_secs, &isecs);

  timeout.tv_sec  = now.tv_sec  + (long)floor( isecs);
  timeout.tv_nsec = now.tv_nsec + (long)floor( fsecs * 1.0e9);
  
  timeout.tv_sec  += timeout.tv_nsec / 1000000000;
  timeout.tv_nsec %= 1000000000;

  err = 0;
  pthread_mutex_lock( &md2cmds_homing_mutex);
  while( err == 0 && md2cmds_homing_count == -1)
    err = pthread_cond_timedwait( &md2cmds_homing_cond, &md2cmds_homing_mutex, &timeout);

  if( err != 0) {
    if( err != ETIMEDOUT) {
      lslogging_log_message( "md2cmds_home_wait: unexpected error from timedwait: %d  tv_sec %ld   tv_nsec %ld", err, timeout.tv_sec, timeout.tv_nsec);
    }
    pthread_mutex_unlock( &md2cmds_homing_mutex);
    return 1;
  }

  err = 0;
  while( err == 0 && md2cmds_homing_count > 0)
    err = pthread_cond_timedwait( &md2cmds_homing_cond, &md2cmds_homing_mutex, &timeout);
  pthread_mutex_unlock( &md2cmds_homing_mutex);

  if( err != 0) {
    if( err != ETIMEDOUT)
      lslogging_log_message( "md2cmds_home_wait: unexpected error from timedwait: %d", err);
    
    return 1;
  } 
  return 0;
}


/** prepare for new movements
 */
void md2cmds_move_prep() {
  pthread_mutex_lock( &md2cmds_moving_mutex);
  lsredis_setstr( md2cmds_md_status_code, "%s", "4");
  lslogging_log_message( "md2cmds_move_prep: status code %ld", lsredis_getl( md2cmds_md_status_code));
  md2cmds_moving_count = md2cmds_moving_count ? md2cmds_moving_count : -1;
  pthread_mutex_unlock( &md2cmds_moving_mutex);
}

/** Wait for all the motions requested to complete
 *
 *  \param timeout_secs  Double value of seconds to wait
 *
 *  There are two waits involved: First to wait for the first "Moving"
 *  to be seen and second to wait for the last "In Position".
 *  The timeout specified here is the sum of the two.
 *
 * returns 0 on success and 1 if we timedout.
 *
 */
int md2cmds_move_wait( double timeout_secs) {
  double isecs, fsecs;
  struct timespec timeout, now;
  int err;

  clock_gettime( CLOCK_REALTIME, &now);

  fsecs = modf( timeout_secs, &isecs);

  timeout.tv_sec  = now.tv_sec  + (long)floor( isecs);
  timeout.tv_nsec = now.tv_nsec + (long)floor( fsecs * 1.0e9);
  
  timeout.tv_sec  += timeout.tv_nsec / 1000000000;
  timeout.tv_nsec %= 1000000000;

  err = 0;
  pthread_mutex_lock( &md2cmds_moving_mutex);
  while( err == 0 && md2cmds_moving_count == -1)
    err = pthread_cond_timedwait( &md2cmds_moving_cond, &md2cmds_moving_mutex, &timeout);

  if( err == ETIMEDOUT) {
    pthread_mutex_unlock( &md2cmds_moving_mutex);
    return 1;
  }

  err = 0;
  while( err == 0 && md2cmds_moving_count > 0)
    err = pthread_cond_timedwait( &md2cmds_moving_cond, &md2cmds_moving_mutex, &timeout);
  pthread_mutex_unlock( &md2cmds_moving_mutex);

  if( err == ETIMEDOUT)
    return 1;
  return 0;
}

/** returns non-zero if we think a motor is moving, 0 otherwise
 */
int md2cmds_is_moving() {
  int rtn;

  pthread_mutex_lock( &md2cmds_moving_mutex);
  rtn = md2cmds_moving_count != 0;
  pthread_mutex_unlock( &md2cmds_moving_mutex);

  return rtn;
}


double md2cmds_prep_axis( lspmac_motor_t *mp, double pos) {
  double rtn;
  double u2c;
  double neutral_pos;

  pthread_mutex_lock( &(mp->mutex));

  u2c         = lsredis_getd( mp->u2c);
  neutral_pos = lsredis_getd( mp->neutral_pos);

  mp->motion_seen = 0;
  mp->not_done    = 1;

  rtn = u2c   * (pos + neutral_pos);

  pthread_mutex_unlock( &(mp->mutex));

  return rtn;
}



void md2cmds_organs_move_presets( char *pay, char *paz, char *pcy, char *pcz, char *psz) {
  double ay,   az,  cy,  cz,  sz;
  int    cay, caz, ccy, ccz, csz;
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

  cay = md2cmds_prep_axis( apery, ay);
  caz = md2cmds_prep_axis( aperz, az);
  ccy = md2cmds_prep_axis( capy,  cy);
  ccz = md2cmds_prep_axis( capz,  cz);
  csz = md2cmds_prep_axis( scint, sz);
  
  //
  // 170          LS-CAT Move U, V, W, X, Y, Z Absolute
  // 	       	      Q40     = X Value
  //		      Q41     = Y Value
  // 		      Q42     = Z Value
  //		      Q43     = U Value
  //		      Q44     = V Value
  //		      Q45     = W Value
  //
  
  lspmac_SockSendDPline( "organs", "&5 Q40=0 Q41=%d Q42=%d Q43=%d Q44=%d Q45=%d Q100=16 B170R", cay, caz, ccy, ccz, csz);
}


/** Transfer a sample
 *  \param dummy Unused
 */
int md2cmds_transfer( const char *dummy) {
  int nextsample, abort_now;
  double ax, ay, az, cx, cy, horz, vert, oref;
  int err;
  int mmask;
  double move_time;


  lsredis_sendStatusReport( 0, "Processing Sample Transfer Request");

  pthread_mutex_lock( &abort_requested_mutex);
  abort_requested = 0;  // lower the abort flag
  pthread_mutex_unlock( &abort_requested_mutex);

  nextsample = lspg_nextsample_all( &err);
  if( err) {
    lsredis_sendStatusReport( 1, "No sample transfer request found: aborting");
    lslogging_log_message( "md2cmds_transfer: %s", err == 2 ? "query error looking for next sample" : "no sample requested to be transfered, false alarm");
    return 1;
  }
  
  //
  // Wait for motors to stop
  //
  if( md2cmds_is_moving()) {
    lsredis_sendStatusReport( 0, "Waiting for previously requested motions to finish");
    lslogging_log_message( "md2cmds_transfer: Waiting for previous motion to finish");
    if( md2cmds_move_wait( 30.0)) {
      lsredis_sendStatusReport( 1, "Waited for a bit but the previously requested motion did not finish.  Aborting.");
      lslogging_log_message( "md2cmds_transfer: Timed out waiting for previous motion to finish.  Aborting transfer");
      lsevents_send_event( "Transfer Aborted");
      return 1;
    }
  }


  //
  // BLUMax sets up an abort dialogbox here.  TODO: We should figure out how we are going to handle that.
  //

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

  lsredis_sendStatusReport( 0, "Moving MD2 devices to sample transfer position");

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
			      capz,      1, "Cover", 0.0,
			      scint,     1, "Cover", 0.0,
                              blight,    1, NULL,    0.0,
			      blight_ud, 1, NULL,    0.0,
			      fluo,      1, NULL,    0.0,
			      NULL);

  lspg_starttransfer_call( nextsample, lspmac_getBIPosition( sample_detected), ax, ay, az, horz, vert, move_time);

  md2cmds_home_prep();

  //
  // Home Kappa
  //
  lspmac_home1_queue( kappa);

  //
  // Home omega
  //
  lspmac_home1_queue( omega);

  //
  // Wait for the kappa/omega homing routines to finish
  //
  if( md2cmds_home_wait( 30.0)) {
    lsredis_sendStatusReport( 1, "Kappa and/or Omega homing routines timed out.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: kappa/omega homing routines taking too long.  Aborting transfer.");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }

  //
  // Home phi (whatever that means)
  //
  // md2cmds_home_prep();
  // lspmac_home1_queue( phi);

  // Now let's get back to postresql (remember our query so long ago?)
  //
  lspg_starttransfer_wait();
  if( lspg_starttransfer.query_error) {
    lsredis_sendStatusReport( 1, "An database related error occurred trying to start the transfer.  This is neither normal nor OK.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: query error starting transfer");
    lsevents_send_event( "Transfer Aborted");
    lspg_starttransfer_done();
    return 1;
  }


  //
  // It's possible that the sample that's mounted is unknown to the robot.
  // If so then we need to abort after we're done moving stuff
  //
  lslogging_log_message( "md2cmds_transfer: no_rows_returned %d, starttransfer %d",
			 lspg_starttransfer.no_rows_returned, lspg_starttransfer.starttransfer);
  if( lspg_starttransfer.no_rows_returned || lspg_starttransfer.starttransfer != 1)
    abort_now = 1;
  else
    abort_now = 0;

  lspg_starttransfer_done();

 
  //
  // Wait for the homing routines to finish
  //
  //  if( md2cmds_home_wait( 30.0)) {
  //    lsredis_sendStatusReport( 1, "Homing phi timed out.  This is unusual.  Aborting");
  //    lslogging_log_message( "md2cmds_transfer: phi homing routine taking too long.  Aborting transfer.");
  //    lsevents_send_event( "Transfer Aborted");
  //    return 1;
  //  }

  //
  // Wait for all those other motors to stop moving
  //
  err = lspmac_est_move_time_wait( move_time + 10.0,
				   mmask,
				   capz,
				   scint,
				   blight_ud,
				   fluo,
				   NULL);
  if( err) {
    lsredis_sendStatusReport( 1, "Timed out waiting for MD2 to ready itself.  Aborting transfer.");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }


  // TODO: check that all the motors are where we told them to go  
  //

  //
  // see if we have a sample mounted problem (is abort_now misnamed?)
  //
  if( abort_now) {
    lsredis_sendStatusReport( 1, "We don't know where the sample that we think is on the diffractometer should go.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: Apparently there is a sample already mounted but we don't know where it is supposed to go");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }
  
  // refuse to go on if we do not have positive confirmation that the backlight is down and the
  // fluorescence detector is back  (TODO: how about all those organs?)
  //
  if( lspmac_getBIPosition( blight_down) != 1 ||lspmac_getBIPosition( fluor_back) != 1) {
    lsredis_sendStatusReport( 1, "Either the backlight or the fluorescence detector is possibly stuck in the wrong position.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: It looks like either the back light is not down or the fluoescence dectector is not back");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }

  //
  // Wait for the robot to unlock the cryo which signals us that we need to
  // move the cryo back and drop air rights
  //
  lsredis_sendStatusReport( 0, "Waiting for the robot to unlock the cryo position and request air rights.");
  if( lspg_waitcryo_all()) {
    lsredis_sendStatusReport( 1, "Query error waiting for the cyro lock.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: query error waiting for the cryo lock, aborting");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }

  

  // Move the cryo back
  //
  cryo->moveAbs( cryo, 1);
  lspmac_moveabs_wait( cryo, 10.0);

  // simplest query yet!
  lspg_query_push( NULL, NULL, "SELECT px.dropairrights()");

  // wait for the result
  // TODO: find an easy way out of this in case of error
  //
  lsredis_sendStatusReport( 0, "Waiting for the requested sample to be mounted.");
  if( lspg_getcurrentsampleid_wait_for_id( nextsample)) {
    lsredis_sendStatusReport( 1, "Error while waiting for the sample transfer to finish.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: query error waiting for the sample transfer");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }

  // grab the airrights again
  //
  lsredis_sendStatusReport( 0, "Demanding air rights for the MD2.");
  if( lspg_demandairrights_all() ) {
    lsredis_sendStatusReport( 1, "Error while demanding air rights.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: query error while demanding air rights, aborting");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }

  // Return the cryo
  //
  cryo->moveAbs( cryo, 0);
  lspmac_moveabs_wait( cryo, 10.0);

  lsredis_sendStatusReport( 0, "Transfer completed.");
  lsevents_send_event( "Transfer Done");

  return 0;
}


/** Move a motor to the position requested
 *  Returns non zero on error
 */
int md2cmds_moveAbs(
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
  int err, prec;
  double move_time;

  // ignore nothing
  if( ccmd == NULL || *ccmd == 0) {
    return 1;
  }

  // operate on a copy of the string since strtok_r will modify its argument
  //
  cmd = strdup( ccmd);

  // Parse the command string
  //
  ignore = strtok_r( cmd, " ", &ptr);
  if( ignore == NULL) {
    lslogging_log_message( "md2cmds_moveAbs: ignoring blank command '%s'", cmd);
    lsredis_sendStatusReport( 1, "moveAbs ignoring blank command");
    free( cmd);
    return 1;
  }

  // The first string should be "moveAbs" cause that's how we got here.
  // Toss it.
  
  mtr = strtok_r( NULL, " ", &ptr);
  if( mtr == NULL) {
    lslogging_log_message( "md2cmds_moveAbs: missing motor name");
    lsredis_sendStatusReport( 1, "moveAbs motor name not given");
    free( cmd);
    return 1;
  }

  mp = lspmac_find_motor_by_name( mtr);
  if( mp == NULL) {
    lslogging_log_message( "md2cmds_moveAbs: cannot find motor %s", mtr);
    lsredis_sendStatusReport( 1, "moveAbs can't find motor named %s", mtr);
    free( cmd);
    return 1;
  }

  pos = strtok_r( NULL, " ", &ptr);
  if( pos == NULL) {
    lslogging_log_message( "md2cmds_moveAbs: missing position");
    lsredis_sendStatusReport( 1, "moveAbs couldn't figure out where to move %s", mtr);
    free( cmd);
    return 1;
  }


  fpos = strtod( pos, &endptr);
  if( pos == endptr) {
    //
    // Maybe we have a preset.  Give it a whirl
    //
    lsredis_sendStatusReport( 0, "moving  %s to '%s'", mtr, pos);

    err = lspmac_est_move_time( &move_time, NULL,
				mp, 1, pos, 0.0,
				NULL);

    if( err) {
      lsredis_sendStatusReport( 1, "Failed to move motor %s to '%s'", mp->name, pos);
      free( cmd);
      return err;
    }

    err = lspmac_est_move_time_wait( move_time + 10.0, 0, mp, NULL);

    if( err) {
      lsredis_sendStatusReport( 1, "Timed out waiting %.1f seconds for motor %s to finish moving", move_time+10.0, mp->name);
    } else {
      lsredis_sendStatusReport( 0, "%s", "");
    }
    free( cmd);
    return err;
  }

  prec = lsredis_getl( mp->printPrecision);
  if( mp != NULL && mp->moveAbs != NULL) {
    pgpmac_printf( "Moving %s to %f\n", mtr, fpos);
    lsredis_sendStatusReport( 0, "moving  %s to %.*f", mtr, prec, fpos);
    err = lspmac_est_move_time( &move_time, NULL,
				mp, 1, NULL, fpos,
				NULL);
    
    if( err) {
      lsredis_sendStatusReport( 1, "Failed to move motor %s to '%.*f'", mp->name, prec, fpos);
      free( cmd);
      return err;
    }

    err = lspmac_est_move_time_wait( move_time + 10.0, 0, mp, NULL);
    if( err) {
      lsredis_sendStatusReport( 1, "Timed out waiting %.1f seconds for motor %s to finish moving", move_time+10.0, mp->name);
    } else {
      lsredis_sendStatusReport( 0, "%s", "");
    }
  }

  free( cmd);
  return err;
}

/** Move a motor to the position requested
 *  Returns non zero on error
 */
int md2cmds_moveRel(
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
  int err;

  // ignore nothing
  if( ccmd == NULL || *ccmd == 0) {
    return 1;
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
    return 1;
  }

  // The first string should be "moveAbs" cause that's how we got here.
  // Toss it.
  
  mtr = strtok_r( NULL, " ", &ptr);
  if( mtr == NULL) {
    lslogging_log_message( "md2cmds_moveRel: missing motor name");
    free( cmd);
    return 1;
  }

  mp = lspmac_find_motor_by_name( mtr);

  if( mp == NULL) {
    lslogging_log_message( "md2cmds_moveRel: cannot find motor %s", mtr);
    free( cmd);
    return 1;
  }

  pos = strtok_r( NULL, " ", &ptr);
  if( pos == NULL) {
    lslogging_log_message( "md2cmds_moveRel: missing position");
    free( cmd);
    return 1;
  }

  fpos = strtod( pos, &endptr);
  if( pos == endptr) {
    //
    // No incrememtnal position found
    //
    lslogging_log_message( "md2cmds_moveRel: no new position requested");
    return 1;
  }

  if( mp != NULL && mp->moveAbs != NULL) {
    lslogging_log_message( "Moving %s by %f\n", mtr, fpos);
    err = mp->moveAbs( mp, lspmac_getPosition(mp) + fpos);
  }

  free( cmd);
  return err;
}





/** Go to the manual mount phase
 */
int md2cmds_phase_manualMount() {
  double move_time;
  int mmask, err;

  lsevents_send_event( "Mode manualMount Starting");
  //
  // Move stuff
  //
  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
                              kappa,     0, "manualMount", 0.0,
                              omega,     0, "manualMount", 0.0,
                              phi,       0, NULL,          0.0,
                              capz,      1, "Cover",       0.0,
                              scint,     1, "Cover",       0.0,
                              blight,    1, NULL,          0.0,
                              blight_ud, 1, NULL,          0.0,
                              cryo,      1, NULL,          0.0,
                              fluo,      1, NULL,          0.0,
                              zoom,      0, NULL,          1.0,
                              NULL);


  if( err) {
    lsevents_send_event( "Mode manualMount Aborted");
    return err;
  }

  //
  // Wait for motion programs
  //

  err = lspmac_est_move_time_wait( move_time+10.0, mmask,
				   capz,
				   scint,
				   blight_ud,
				   cryo,
				   fluo,
				   NULL);
  if( err) {
    lsevents_send_event( "Mode manualMount Aborted");
    return err;
  }

  lsevents_send_event( "Mode manualMount Done");
  return 0;
}


/** Go to robot mount phase
 *  Normally this would not be called as md2cmds_transfer would put things into the correct position
 *  If you need to change the behaviour of this function be sure to change md2cmds_transfer as well.
*/
int md2cmds_phase_robotMount() {
  double move_time;
  int mmask, err;

  lsevents_send_event( "Mode robotMount Starting");

  md2cmds_home_prep();

  //
  // Move 'em
  //

  lspmac_home1_queue( kappa);
  lspmac_home1_queue( omega);
  lspmac_home1_queue( kappa);

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
                              apery,     1, "In",    0.0,
                              aperz,     1, "In",    0.0,
                              capz,      1, "Cover", 0.0,
                              scint,     1, "Cover", 0.0,
                              blight,    1, NULL,    0.0,
                              blight_ud, 1, NULL,    0.0,
                              cryo,      1, NULL,    0.0,
                              fluo,      1, NULL,    0.0,
                              zoom,      0, NULL,    1.0,
                              NULL);

  err = lspmac_est_move_time_wait( move_time + 10.0, mmask,
				   apery,
				   aperz,
				   capz,
				   scint,
				   blight_ud,
				   cryo,
				   fluo,
				   NULL);
  if( err) {
    lsevents_send_event( "Mode robotMount Aborted");
    return err;
  }

  err = md2cmds_home_wait( 60.0);
  if( err) {
    lslogging_log_message( "md2cmds_phase_robotMount: timed out homing omega or kappa");
    lsevents_send_event( "Mode robotMount Aborted");
    return err;
  }

  md2cmds_home_prep();
  lspmac_home1_queue( phi);
  err = md2cmds_home_wait( 60.0);
  if( err) {
    lslogging_log_message( "md2cmds_phase_robotMount: timed out homing phi");
    lsevents_send_event( "Mode robotMount Aborted");
    return err;
  }


  lsevents_send_event( "Mode robotMount Done");
  return 0;
}

/** Go to center phase
 */

int md2cmds_phase_center() {
  double move_time;
  int mmask, err;

  lsevents_send_event( "Mode center Starting");
  //
  // Move 'em
  //

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
			      alignx,    0, "Beam", 0.0,
			      aligny,    0, "Beam", 0.0,
			      alignz,    0, "Beam", 0.0,
			      cenx,      0, "Beam", 0.0,
			      ceny,      0, "Beam", 0.0,
                              kappa,     0, NULL,    0.0,
                              phi,       0, NULL,    0.0,
                              apery,     0, "In",    0.0,
                              aperz,     0, "Out",    0.0,
                              capy,      0, "In",    0.0,
                              capz,      0, "Out",    0.0,
                              scint,     0, "Cover", 0.0,
                              blight_ud, 1, NULL,    1.0,
                              zoom,      0, NULL,    1.0,
                              cryo,      1, NULL,    0.0,
                              fluo,      1, NULL,    0.0,
                              NULL);
  if( err) {
    lsevents_send_event( "Mode center Aborted");
    return err;
  }

  err = lspmac_est_move_time_wait( move_time + 10.0, mmask,
				   cryo,
				   fluo,
				   NULL);
  if( err) {
    lsevents_send_event( "Mode center Aborted");
    return err;
  }

  lsevents_send_event( "Mode center Done");
  return 0;
}


/** Go to data collection phase
 */
int md2cmds_phase_dataCollection() {
  double move_time;
  int mmask, err;

  lsevents_send_event( "Mode dataCollection Starting");

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
			      alignx,    0, "Beam", 0.0,
			      aligny,    0, "Beam", 0.0,
			      alignz,    0, "Beam", 0.0,
			      cenx,      0, "Beam", 0.0,
			      ceny,      0, "Beam", 0.0,
                              apery,     1, "In",      0.0,
                              aperz,     1, "In",      0.0,
                              capy,      1, "In",      0.0,
                              capz,      1, "In",      0.0,
                              scint,     1, "Cover",   0.0,
                              blight,    1, NULL,      0.0,
                              blight_ud, 1, NULL,      0.0,
                              cryo,      1, NULL,      0.0,
                              fluo,      1, NULL,      0.0,
                              NULL);
  if( err) {
    lsevents_send_event( "Mode dataCollection Aborted");
    return err;
  }

  err = lspmac_est_move_time_wait( move_time + 10.0, mmask,
				   apery,
				   aperz,
				   capy,
				   capz,
				   scint,
				   blight_ud,
				   cryo,
				   fluo,
				   NULL);
  if( err) {
    lsevents_send_event( "Mode dataCollection Aborted");
    return err;
  }

  lsevents_send_event( "Mode dataCollection Done");
  return 0;
}


/** Go to beam location phase
 */
int md2cmds_phase_beamLocation() {
  double move_time;
  int mmask, err;

  lsevents_send_event( "Mode beamLocation Starting");

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
                              //motor   jog, preset,      position if no preset
			      kappa,      0, NULL,           0.0,
                              apery,      0, "In",           0.0,
                              aperz,      0, "In",           0.0,
                              capy,       0, "In",           0.0,
                              capz,       0, "In",           0.0,
                              scint,      0, "Scintillator", 0.0,
                              blight,     1, NULL,           0.0,
                              blight_ud,  1, NULL,           0.0,
                              zoom,       0, NULL,           1.0,
                              cryo,       1, NULL,           0.0,
                              fluo,       1, NULL,           0.0,
                              NULL);
  if( err) {
    lsevents_send_event( "Mode beamLocation Aborted");
    return err;
  }

  err = lspmac_est_move_time_wait( move_time + 10.0, mmask,
				   blight_ud,
				   cryo,
				   fluo,
				   NULL);
  if( err) {
    lsevents_send_event( "Mode beamLocation Aborted");
    return err;
  }

  lsevents_send_event( "Mode beamLocation Done");
  return 0;
}


/** Go to safe phase
 */
int md2cmds_phase_safe() {
  double move_time;
  int mmask, err;

  lsevents_send_event( "Mode safe Starting");

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
                              //motor   jog, preset,      position if no preset
                              kappa,      0, NULL,           0.0,
                              apery,      1, "In",           0.0,
                              aperz,      1, "Cover",        0.0,
                              capy,       1, "In",           0.0,
                              capz,       1, "Cover",        0.0,
                              scint,      1, "Cover",        0.0,
                              blight,     1, NULL,           0.0,
                              blight_ud,  1, NULL,           0.0,
                              zoom,       0, NULL,           1.0,
                              cryo,       1, NULL,           0.0,
                              fluo,       1, NULL,           0.0,
                              NULL);


  if( err) {
    lsevents_send_event( "Mode safe Aborted");
    return err;
  }

  err = lspmac_est_move_time_wait( move_time + 10.0, mmask,
				   apery,
				   aperz,
				   capy,
				   capz,
				   scint,
				   blight_ud,
				   cryo,
				   fluo,
				   NULL);
  if( err) {
    lsevents_send_event( "Mode safe Aborted");
    return err;
  }

  lsevents_send_event( "Mode safe Done");
  return 0;
}



/** Move md2 devices to a preconfigured state.
 *  EMBL calls these states "phases" and this language is partially retained here
 *
 *  \param ccmd The full text of the command that sent us here
 */
int md2cmds_phase_change( const char *ccmd) {
  char *cmd;
  char *ignore;
  char *ptr;
  char *mode;
  int err;

  if( ccmd == NULL || *ccmd == 0)
    return 1;

  // use a copy as strtok_r modifies the string it is parsing
  //

  cmd = strdup( ccmd);

  ignore = strtok_r( cmd, " ", &ptr);
  if( ignore == NULL) {
    lslogging_log_message( "md2cmds_phase_change: ignoring empty command string (how did we let things get this far?");
    free( cmd);
    return 1;
  }

  //
  // ignore should point to "mode" cause that's how we got here.  Ignore it
  //
  mode = strtok_r( NULL, " ", &ptr);
  if( mode == NULL) {
    lslogging_log_message( "md2cmds_phase_change: no mode specified");
    return 1;
  }

  if( md2cmds_is_moving()) {
    int err;
    lspmac_SockSendDPControlChar( "Aborting Motions", '\x01');
    err = md2cmds_move_wait( 2.0);
    if( err) {
      lslogging_log_message( "md2cmds_phase_change: Timed out waiting for previous moves to finish");
      return 1;
    }
  }

  //
  // Tangled web.  Probably not worth fixing.  O(N) but N is 6.
  //

  err = 1;
  if( strcmp( mode, "manualMount") == 0) {
    err = md2cmds_phase_manualMount();
  } else if( strcmp( mode, "robotMount") == 0) {
    err = md2cmds_phase_robotMount();
  } else if( strcmp( mode, "center") == 0) {
    err = md2cmds_phase_center();
  } else if( strcmp( mode, "dataCollection") == 0) {
    err = md2cmds_phase_dataCollection();
  } else if( strcmp( mode, "beamLocation") == 0) {
    err = md2cmds_phase_beamLocation();
  } else if( strcmp( mode, "safe") == 0) {
    err = md2cmds_phase_safe();
  }

  lsredis_setstr( lsredis_get_obj( "phase"), err ? "unknown" : mode);

  free( cmd);
  return err;
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

  cx_cts = md2cmds_prep_axis( cenx,   cx);
  cy_cts = md2cmds_prep_axis( ceny,   cy);
  ax_cts = md2cmds_prep_axis( alignx, ax);
  ay_cts = md2cmds_prep_axis( aligny, ay);
  az_cts = md2cmds_prep_axis( alignz, az);

  lspmac_SockSendDPline( NULL, "&2 Q100=2 Q20=%.1f Q21=%.1f B150R", cx_cts, cy_cts);
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
  lslogging_log_message( "md2cmds_maybe_done_moving_cb: status code %ld", lsredis_getl( md2cmds_md_status_code));
  
  if( md2cmds_moving_count == 0)
    pthread_cond_signal( &md2cmds_moving_cond);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
  
}


/** Track motors homing
 */
void md2cmds_maybe_done_homing_cb( char *event) {
  pthread_mutex_lock( &md2cmds_homing_mutex);
  
  if( strstr( event, "Homing") != NULL) {
    if( md2cmds_homing_count == -1)
      md2cmds_homing_count = 1;
    else
      md2cmds_homing_count++;
  } else {
    if( md2cmds_homing_count > 0)
      md2cmds_homing_count--;
  }

  if( md2cmds_homing_count != 0)
    lsredis_setstr( md2cmds_md_status_code, "%s", "4");

  if( md2cmds_homing_count == 0)
    pthread_cond_signal( &md2cmds_homing_cond);
  
  pthread_mutex_unlock( &md2cmds_homing_mutex);
}



void md2cmds_kappaphi_move( double kappa_deg, double phi_deg) {
  int kc, pc;

  // coordinate system 7
  // 1 << (coord sys no - 1) = 64

  kc = md2cmds_prep_axis( kappa, kappa_deg);
  pc = md2cmds_prep_axis( kappa, phi_deg);

  //  ;150		LS-CAT Move X, Y Absolute
  //  ;			Q20    = X Value
  //  ;			Q21    = Y Value
  //  ;			Q100   = 1 << (coord sys no  - 1)

  lspmac_SockSendDPline( "kappaphi_move", "&7 Q20=%d Q21=%d Q100=64", kc, pc);
}


/** Collect some data
 *  \param dummy Unused
 *  returns non-zero on error
 */
int md2cmds_collect( const char *dummy) {
  long long skey;	//!< index of shot to be taken
  double exp_time;      //!< Exposure time (saved to compute shutter timeout)
  double p170;		//!< start cnts
  double p171;		//!< delta cnts
  double p173;		//!< omega velocity cnts/msec
  double p175;		//!< acceleration time (msec)
  double p180;		//!< exposure time (msec)
  double u2c;		//!< unit to counts conversion
  double neutral_pos;	//!< nominal zero offset
  double max_accel;	//!< maximum acceleration allowed for omega
  double kappa_pos;	//!< current kappa position in case we need to move phi only
  double phi_pos;	//!< current phi position in case we need to move kappa only
  struct timespec now, timeout;	//!< setup timeouts for shutter
  int err;
  double move_time;
  int mmask;
  lsredis_obj_t *collection_running;

  lsevents_send_event( "Data Collection Starting");

  collection_running = lsredis_get_obj( "collection.running");
  lsredis_setstr( collection_running, "True");

  u2c         = lsredis_getd( omega->u2c);
  neutral_pos = lsredis_getd( omega->neutral_pos);
  max_accel   = lsredis_getd( omega->max_accel);

  mmask = 0;


  //
  // Go to data collection mode
  //
  lsredis_sendStatusReport( 0, "Putting MD2 in data collection mode");
  if( md2cmds_phase_change( "changeMode dataCollection")) {
    lsredis_sendStatusReport( 1, "Failed to put MD2 into data collection mode within a reasonable amount of time.");
    lsevents_send_event( "Data Collection Aborted");
    lsredis_setstr( collection_running, "False");
    return 1;
  }  


  //
  // reset shutter has opened flag
  //
  lspmac_SockSendDPline( NULL, "P3001=0 P3002=0");

  while( 1) {
    lspg_nextshot_call();
    lspg_nextshot_wait();

    if( lspg_nextshot.query_error) {
      lsredis_sendStatusReport( 1, "Could not retrieve next shot info.");
      lslogging_log_message( "md2cmds_collect: query error retrieving next shot info.  aborting");
      lsevents_send_event( "Data Collection Aborted");
      lspg_nextshot_done();
      lsredis_setstr( collection_running, "False");
      return 1;
    }

    if( lspg_nextshot.no_rows_returned) {
      lsredis_sendStatusReport( 0, "No more images to collect");
      lspg_nextshot_done();
      break;
    }

    exp_time = lspg_nextshot.dsexp;

    skey = lspg_nextshot.skey;
    lslogging_log_message( "md2cmds next shot is %lld", skey);
    lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Preparing')", skey);
    lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Preparing\"}", skey);
    lsredis_sendStatusReport( 0, "Preparing...");

    if( lspg_nextshot.active) {
      if(
	 //
	 // Don't move if we are within 0.1 microns of our destination
	 //
	 (fabs( lspg_nextshot.cx - cenx->position) > 0.0001) ||
	 (fabs( lspg_nextshot.cy - ceny->position) > 0.0001) ||
	 (fabs( lspg_nextshot.ax - alignx->position) > 0.0001) ||
	 (fabs( lspg_nextshot.ay - aligny->position) > 0.0001) ||
	 (fabs( lspg_nextshot.az - alignz->position) > 0.0001)) {


	lslogging_log_message( "md2cmds_collect: moving center to cx=%f, cy=%f, ax=%f, ay=%f, az=%f",lspg_nextshot.cx, lspg_nextshot.cy, lspg_nextshot.ax, lspg_nextshot.ay, lspg_nextshot.az);

	err = lspmac_est_move_time( &move_time, &mmask,
				    cenx,   0, NULL, lspg_nextshot.cx,
				    ceny,   0, NULL, lspg_nextshot.cy,
				    alignx, 0, NULL, lspg_nextshot.ax,
				    aligny, 0, NULL, lspg_nextshot.ay,
				    alignz, 0, NULL, lspg_nextshot.az,
				    NULL);
	if( err) {
	  lsevents_send_event( "Data Collection Aborted");
	  lsredis_sendStatusReport( 1, "Failed to start moving to next sample position.");
	  lspg_nextshot_done();
	  lsredis_setstr( collection_running, "False");
	  return 1;
	}

	err = lspmac_est_move_time_wait( move_time+10, mmask, NULL);
	if( err) {
	  lsredis_sendStatusReport( 1, "Moving to next sample position failed.");
	  lsevents_send_event( "Data Collection Aborted");
	  //	  lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");   // Should we even have the diffractometer lock at this point?
	  lspg_nextshot_done();
	  lsredis_setstr( collection_running, "False");
	  return 1;
	}
      }
    }

    // Maybe move kappa and/or phi
    //
    if( !lspg_nextshot.dsphi_isnull || !lspg_nextshot.dskappa_isnull) {

      kappa_pos = lspg_nextshot.dskappa_isnull ? lspmac_getPosition( kappa) : lspg_nextshot.dskappa;
      phi_pos   = lspg_nextshot.dsphi_isnull   ? lspmac_getPosition( phi)   : lspg_nextshot.dsphi;

      lsredis_sendStatusReport( 0, "Moving Kappa");
      lslogging_log_message( "md2cmds_collect: move phy/kappa: kappa=%f  phi=%f", kappa_pos, phi_pos);

      err = lspmac_est_move_time( &move_time, &mmask,
				  kappa, 0, NULL, kappa_pos,
				  phi,   0, NULL, phi_pos,
				  NULL);
      if( err) {
	lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
	lsevents_send_event( "Data Collection Aborted");
	lsredis_sendStatusReport( 1, "Moving Kappa failed");
	lspg_nextshot_done();
	lsredis_setstr( collection_running, "False");
	return 1;
      }	

      err = lspmac_est_move_time_wait( move_time + 10, mmask, NULL);
      if( err) {
	lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
	lsevents_send_event( "Data Collection Aborted");
	lsredis_sendStatusReport( 1, "Moving Kappa timed out");
	lspg_nextshot_done();
	lsredis_setstr( collection_running, "False");
	return 1;
      }	
    }

  
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
    if( lspg_seq_run_prep_all( skey,
			       kappa->position,
			       phi->position,
			       cenx->position,
			       ceny->position,
			       alignx->position,
			       aligny->position,
			       alignz->position
			       )) {
      lslogging_log_message( "md2cmds_collect: seq run prep query error, aborting");
      lsredis_sendStatusReport( 1, "Preparing MD2 failed");
      lsevents_send_event( "Data Collection Aborted");
      lsredis_setstr( collection_running, "False");
      return 1;
    }
    
    //
    // make sure our opened flag is down
    // wait for the p3001=0 command to be noticed
    //
    clock_gettime( CLOCK_REALTIME, &now);
    timeout.tv_sec  = now.tv_sec + 10;
    timeout.tv_nsec = now.tv_nsec;

    err = 0;
    pthread_mutex_lock( &lspmac_shutter_mutex);
    while( err == 0 && lspmac_shutter_has_opened != 0)
      err = pthread_cond_timedwait( &lspmac_shutter_cond, &lspmac_shutter_mutex, &timeout);
    pthread_mutex_unlock( &lspmac_shutter_mutex);

    if( err == ETIMEDOUT) {
      pthread_mutex_unlock( &lspmac_shutter_mutex);
      lsredis_sendStatusReport( 1, "Timed out waiting for shutter closed confirmation.");
      lslogging_log_message( "md2cmds_collect: Timed out waiting for shutter to be confirmed closed.  Data collection aborted.");
      lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
      lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");
      lsevents_send_event( "Data Collection Aborted");
      lsredis_setstr( collection_running, "False");
      return 1;
    }

    //
    // Wait for the detector to drop its lock indicating that it is ready for the exposure
    //
    lspg_lock_detector_all();
    lspg_unlock_detector_all();


    //
    // Start the exposure
    //
    lsredis_sendStatusReport( 0, "Exposing.");
    lspmac_set_motion_flags( &mmask, omega, NULL);
    lspmac_SockSendDPline( "Exposure",
			   "&1 P170=%.1f P171=%.1f P173=%.1f P174=0 P175=%.1f P176=0 P177=1 P178=0 P180=%.1f M431=1 &1B131R",
			   p170,         p171,     p173,            p175,                          p180);

    //
    // We could look for the "Exposure command accepted" event at this point.
    //
    //
    // wait for the shutter to open
    //
    clock_gettime( CLOCK_REALTIME, &now);
    timeout.tv_sec  = now.tv_sec + 10;
    timeout.tv_nsec = now.tv_nsec;

    err = 0;
    pthread_mutex_lock( &lspmac_shutter_mutex);
    while( err == 0 && lspmac_shutter_has_opened == 0)
      err = pthread_cond_timedwait( &lspmac_shutter_cond, &lspmac_shutter_mutex, &timeout);

    if( err == ETIMEDOUT) {
      pthread_mutex_unlock( &lspmac_shutter_mutex);
      lslogging_log_message( "md2cmds_collect: Timed out waiting for shutter to open.  Data collection aborted.");
      lsredis_sendStatusReport( 1, "Timed out waiting for shutter to open.");
      lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");
      lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
      lsevents_send_event( "Data Collection Aborted");
      lsredis_setstr( collection_running, "False");
      return 1;
    }


    //
    // wait for the shutter to close
    //
    clock_gettime( CLOCK_REALTIME, &now);
    lslogging_log_message( "md2cmds_collect: waiting %f seconds for the shutter to close", 4 + exp_time);
    timeout.tv_sec  = now.tv_sec + 4 + ceil(exp_time);	// hopefully 4 seconds is long enough to never catch a legitimate shutter close and short enough to bail when something is really wrong
    timeout.tv_nsec = now.tv_nsec;

    err = 0;
    while( err == 0 && lspmac_shutter_state == 1)
      err = pthread_cond_timedwait( &lspmac_shutter_cond, &lspmac_shutter_mutex, &timeout);
    pthread_mutex_unlock( &lspmac_shutter_mutex);


    if( err == ETIMEDOUT) {
      pthread_mutex_unlock( &lspmac_shutter_mutex);
      lsredis_sendStatusReport( 1, "Timed out waiting for shutter to close.");
      lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");
      lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
      lslogging_log_message( "md2cmds_collect: Timed out waiting for shutter to close.  Data collection aborted.");
      lsevents_send_event( "Data Collection Aborted");
      lsredis_setstr( collection_running, "False");
      return 1;
    }


    //
    // Signal the detector to start reading out
    //
    lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");
    lsredis_sendStatusReport( 0, "Reading out image.");

    //
    // Update the shot status
    //
    lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Writing')", skey);

    //
    // reset shutter has opened flag
    //
    lspmac_SockSendDPline( NULL, "P3001=0");

    //
    // Wait for omega to stop moving
    //
    if( md2cmds_move_wait( 10.0)) {
      lslogging_log_message( "md2cmds_collect: Giving up waiting for omega to stop moving. Data collection aborted.");
      lsredis_sendStatusReport( 1, "Timed out waiting for omega to stop.");
      lsevents_send_event( "Data Collection Aborted");
      lsredis_setstr( collection_running, "False");
      return 1;
    }

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

	md2cmds_move_prep();
	md2cmds_mvcenter_move( lspg_nextshot.cx, lspg_nextshot.cy, lspg_nextshot.ax, lspg_nextshot.ay, lspg_nextshot.az);
      }
    }
  }
  lsevents_send_event( "Data Collection Done");
  lsredis_sendStatusReport( 0, "");
  lsredis_setstr( collection_running, "False");
  return 0;
}

/** Spin 360 and make a video (recenter first, maybe)
 *  \param dummy Unused
 *  returns non-zero on error
 *  
 */
int md2cmds_rotate( const char *dummy) {
  double cx, cy, ax, ay, az,   zm;
  double        bax, bay, baz;
  int mmask;
  int err;
  double move_time;

  mmask = 0;
  //
  // BLUMax disables scintilator here.
  //

  //
  // get the new center information
  //
  lspg_getcenter_call();
  lspg_getcenter_wait();

  if( lspg_getcenter.query_error) {
    lslogging_log_message( "md2cmds_rotate: get center query error, aborting");
    lsevents_send_event( "Rotate Aborted");
    lspg_getcenter_done();
    return 1;
  }


  // put up the back light
  blight_ud->moveAbs( blight_ud, 1);

  //
  // Get ready to move our motors
  md2cmds_home_prep();

  //
  // make sure omega is homed
  //
  lspmac_home1_queue( omega);

  //
  // Grab the current positions
  //
  cx = lspmac_getPosition( cenx);
  cy = lspmac_getPosition( ceny);
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);

  lslogging_log_message( "md2cmds_rotate: actual positions cx %f, cy %f, ax %f, ay %f, az %f", cx, cy, ax, ay, az);

  if( lspg_getcenter.no_rows_returned) {
    //
    // Always specify zoom even if no other center information is found
    //
    zm = 1;
  } else {
    lslogging_log_message( "md2cmds_rotate: getcenter returned dcx %f, dcy %f, dax %f, day %f, daz %f, zoom %d",
			   lspg_getcenter.dcx, lspg_getcenter.dcy, lspg_getcenter.dax, lspg_getcenter.day, lspg_getcenter.daz,lspg_getcenter.zoom);

    if( lspg_getcenter.zoom_isnull == 0) {
      zm = lspg_getcenter.zoom;
    } else {
      zm = 1.0;
    }

    if( lspg_getcenter.dcx_isnull == 0)
      cx += lspg_getcenter.dcx;

    if( lspg_getcenter.dcy_isnull == 0)
      cy  += lspg_getcenter.dcy;

    //
    // Slightly complicated procedure for alignment stage since we might want to update
    // the presets.  Use the preset Back_Vector to calculate the new Back preset from our
    // current position.
    //
    if( lspg_getcenter.dax_isnull == 0) {
      err = lsredis_find_preset( "align.x", "Back_Vector", &bax);
      if( err == 0)
	bax = 0.0;
      bax += lspg_getcenter.dax;
      lsredis_set_preset( "align.x", "Back", bax);

      ax  += lspg_getcenter.dax;
      lsredis_set_preset( "align.x", "Beam", ax);
    }      


    if( lspg_getcenter.day_isnull == 0) {
      err = lsredis_find_preset( "align.y", "Back_Vector", &bay);
      if( err == 0)
	bay = 0.0;
      bay += lspg_getcenter.day;
      lsredis_set_preset( "align.y", "Back", bay);

      ay  += lspg_getcenter.day;
      lsredis_set_preset( "align.y", "Beam", ay);
    }
			  
    if( lspg_getcenter.daz_isnull == 0) {
      err = lsredis_find_preset( "align.z", "Back_Vector", &baz);
      if( err == 0)
	baz = 0.0;
      baz += lspg_getcenter.daz;
      lsredis_set_preset( "align.z", "Back", baz);

      az  += lspg_getcenter.daz;
      lsredis_set_preset( "align.z", "Beam", az);
    }
  }
  lspg_getcenter_done();

  if( lspmac_est_move_time( &move_time, &mmask,
			    scint,  0,  "Cover", 0.0,
			    capz,   0,  "Cover", 0.0,
			    cenx,   0,  NULL,    cx,
			    ceny,   0,  NULL,    cy,
			    alignx, 0,  NULL,    ax,
			    aligny, 0,  NULL,    ay,
			    alignz, 0,  NULL,    az,
			    zoom,   1,  NULL,    zm,
			    NULL)) {
    lslogging_log_message( "md2cmds_rotate: organ motion request failed");
    lsevents_send_event( "Rotate Aborted");
    return 1;
  }

  if( lspmac_est_move_time_wait( move_time + 10.0, mmask,
				 zoom,
				 NULL)) {
    lslogging_log_message( "md2cmds_rotate: organ motion timed out %f seconds", move_time + 10.0);
    lsevents_send_event( "Rotate Aborted");
    return 1;
  }

  if( md2cmds_home_wait( 20.0)) {
    lslogging_log_message( "md2cmds_rotate: homing motors timed out.  Rotate aborted");
    lsevents_send_event( "Rotate Aborted");
    return 1;
  }


  // Report new center positions
  cx = lspmac_getPosition( cenx);
  cy = lspmac_getPosition( ceny);
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);
  lspg_query_push( NULL, NULL, "SELECT px.applycenter( %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f)", cx, cy, ax, ay, az, lspmac_getPosition(kappa), lspmac_getPosition( phi));

  lslogging_log_message( "md2cmds_rotate: done with applycenter");
  lspmac_video_rotate( 4.0);
  lslogging_log_message( "md2cmds_rotate: starting rotation");
  rotating = 1;

  return 0;
}

/** Tell the database about the time we went through omega=zero.
 *  This should trigger the video feed server to starting making a movie.
 */
void md2cmds_rotate_cb( char *event) {
  static lsredis_obj_t *ozt  = NULL;
  struct tm t;
  int usecs;

  gmtime_r( &(omega_zero_time.tv_sec), &t);
  
  usecs = omega_zero_time.tv_nsec / 1000;
  lspg_query_push( NULL, NULL, "SELECT px.trigcam('%d-%d-%d %d:%d:%d.%06d', %d, 0.0, 90.0)",
		   t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, usecs,
		   (int)(lspmac_getPosition( zoom)));

  if( ozt == NULL)
    ozt = lsredis_get_obj( "omega.rotate.time");

  lsredis_setstr( ozt, "{\"timestamp\": \"%04d-%02d-%02dT%02d:%02d:%02d.%06dZ\", \"zoom\": %d, \"angle\": 0.0, \"velocity\": 90.0, \"hash\": \"%s\"}",
		  t.tm_year+1900, t.tm_mon+1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec, usecs,
		  (int)(lspmac_getPosition( zoom)), (lspg_getcenter.hash == NULL ? "unknown" : lspg_getcenter.hash));
  
}

/** Now that we are done with the 360 rotation lets rehome right quick
 */
void md2cmds_maybe_rotate_done_cb( char *event) {
  if( rotating) {
    rotating = 0;
    lsevents_send_event( "Rotate Done");
  }
}



/** Do not spin 360 or make a video
 * For local centering without a movie
 *  \param dummy Unused
 *  returns non-zero on error
 *  
 */
int md2cmds_nonrotate( const char *dummy) {
  double cx, cy, ax, ay, az,   zm;
  double        bax, bay, baz;
  int mmask;
  int err;
  double move_time;

  mmask = 0;

  //
  // get the new center information
  //
  lspg_getcenter_call();
  lspg_getcenter_wait();

  if( lspg_getcenter.query_error) {
    lslogging_log_message( "md2cmds_nonrotate: get center query error, aborting");
    lsevents_send_event( "Local Centering Aborted");
    lspg_getcenter_done();
    return 1;
  }


  // put up the back light
  blight_ud->moveAbs( blight_ud, 1);

  //
  // Get ready to move our motors
  md2cmds_home_prep();

  //
  // make sure omega is homed
  //
  lspmac_home1_queue( omega);

  //
  // Grab the current positions
  //
  cx = lspmac_getPosition( cenx);
  cy = lspmac_getPosition( ceny);
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);

  lslogging_log_message( "md2cmds_nonrotate: actual positions cx %f, cy %f, ax %f, ay %f, az %f", cx, cy, ax, ay, az);

  if( lspg_getcenter.no_rows_returned) {
    //
    // Always specify zoom even if no other center information is found
    //
    zm = 1;
  } else {
    lslogging_log_message( "md2cmds_nonrotate: getcenter returned dcx %f, dcy %f, dax %f, day %f, daz %f, zoom %d",
			   lspg_getcenter.dcx, lspg_getcenter.dcy, lspg_getcenter.dax, lspg_getcenter.day, lspg_getcenter.daz,lspg_getcenter.zoom);

    if( lspg_getcenter.zoom_isnull == 0) {
      zm = lspg_getcenter.zoom;
    } else {
      zm = 1.0;
    }

    if( lspg_getcenter.dcx_isnull == 0)
      cx += lspg_getcenter.dcx;

    if( lspg_getcenter.dcy_isnull == 0)
      cy  += lspg_getcenter.dcy;

    //
    // Slightly complicated procedure for alignment stage since we might want to update
    // the presets.  Use the preset Back_Vector to calculate the new Back preset from our
    // current position.
    //
    if( lspg_getcenter.dax_isnull == 0) {
      err = lsredis_find_preset( "align.x", "Back_Vector", &bax);
      if( err == 0)
	bax = 0.0;
      bax += lspg_getcenter.dax;
      lsredis_set_preset( "align.x", "Back", bax);

      ax  += lspg_getcenter.dax;
      lsredis_set_preset( "align.x", "Beam", ax);
    }      


    if( lspg_getcenter.day_isnull == 0) {
      err = lsredis_find_preset( "align.y", "Back_Vector", &bay);
      if( err == 0)
	bay = 0.0;
      bay += lspg_getcenter.day;
      lsredis_set_preset( "align.y", "Back", bay);

      ay  += lspg_getcenter.day;
      lsredis_set_preset( "align.y", "Beam", ay);
    }
			  
    if( lspg_getcenter.daz_isnull == 0) {
      err = lsredis_find_preset( "align.z", "Back_Vector", &baz);
      if( err == 0)
	baz = 0.0;
      baz += lspg_getcenter.daz;
      lsredis_set_preset( "align.z", "Back", baz);

      az  += lspg_getcenter.daz;
      lsredis_set_preset( "align.z", "Beam", az);
    }
  }
  lspg_getcenter_done();

  if( lspmac_est_move_time( &move_time, &mmask,
			    scint,  0,  "Cover", 0.0,
			    capz,   0,  "Cover", 0.0,
			    cenx,   0,  NULL,    cx,
			    ceny,   0,  NULL,    cy,
			    alignx, 0,  NULL,    ax,
			    aligny, 0,  NULL,    ay,
			    alignz, 0,  NULL,    az,
			    zoom,   1,  NULL,    zm,
			    NULL)) {
    lslogging_log_message( "md2cmds_nonrotate: organ motion request failed");
    lsevents_send_event( "Local Centering Aborted");
    return 1;
  }

  if( lspmac_est_move_time_wait( move_time + 10.0, mmask,
				 zoom,
				 NULL)) {
    lslogging_log_message( "md2cmds_nonrotate: organ motion timed out %f seconds", move_time + 10.0);
    lsevents_send_event( "Local Centering Aborted");
    return 1;
  }

  if( md2cmds_home_wait( 20.0)) {
    lslogging_log_message( "md2cmds_nonrotate: homing motors timed out.  Rotate aborted");
    lsevents_send_event( "Local Centering Aborted");
    return 1;
  }


  // Report new center positions
  cx = lspmac_getPosition( cenx);
  cy = lspmac_getPosition( ceny);
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);
  lspg_query_push( NULL, NULL, "SELECT px.applycenter( %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f)", cx, cy, ax, ay, az, lspmac_getPosition(kappa), lspmac_getPosition( phi));

  lslogging_log_message( "md2cmds_nonrotate: done with applycenter");

  return 0;
}

/** Fix up xscale and yscale when zoom changes
 *  xscale and yscale have units of microns per pixel
 */
void md2cmds_set_scale_cb( char *event) {
  int mag;
  lsredis_obj_t *p1, *p2;
  char *vp;

  pthread_mutex_lock( &zoom->mutex);
  mag = zoom->requested_position;
  pthread_mutex_unlock( &zoom->mutex);
  if( mag <=0 || mag >10) {
    return;
  }

  p1  = lsredis_get_obj( "cam.xScale");
  p2  = lsredis_get_obj( "cam.zoom.%d.ScaleX", mag);

  vp = lsredis_getstr( p2);
  lsredis_setstr( p1, vp);
  free( vp);

  p1  = lsredis_get_obj( "cam.CenterX");
  p2  = lsredis_get_obj( "cam.zoom.%d.CenterX", mag);

  vp = lsredis_getstr( p2);
  lsredis_setstr( p1, vp);
  free( vp);

  p1  = lsredis_get_obj( "cam.yScale");
  p2  = lsredis_get_obj( "cam.zoom.%d.ScaleY", mag);

  vp = lsredis_getstr( p2);
  lsredis_setstr( p1, vp);
  free( vp);

  p1  = lsredis_get_obj( "cam.CenterY");
  p2  = lsredis_get_obj( "cam.zoom.%d.CenterY", mag);

  vp = lsredis_getstr( p2);
  lsredis_setstr( p1, vp);
  free( vp);
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

/** abort the current motion and put the system into a known state
 *  /param dummy Unused here
 */
int md2cmds_abort( const char *dummy) {
  //
  // First priority is to close the shutter
  //
  if( fshut->moveAbs( fshut, 0))
    lslogging_log_message( "md2cmds_abort: for some reason the shutter close requested failed.  Proceeding anyway.");

  //
  // Now stop all the motors
  //
  lspmac_abort();
  if( md2cmds_move_wait( 10.0))
    lslogging_log_message( "md2cmds_abort: Some motors did not appear to stop.  Proceding with reset anyway");

  //
  // Now try to close the shutter (again)
  //
  if( fshut->moveAbs( fshut, 0))
    lslogging_log_message( "md2cmds_abort: for some reason the shutter close requested failed (2).  Proceeding anyway.");
  
  //
  // Force the motion flags down
  //
  lspmac_SockSendDPline( NULL, "m5075=0");
  
  return 0;
}

/** pause until md2cmds_worker has finished running the command
 */
void md2cmds_action_wait() {
  pthread_mutex_lock( &md2cmds_mutex);
  pthread_mutex_unlock( &md2cmds_mutex);
}

/** Run the test routine(s)
 *  \param dummy Unused
 */
int md2cmds_test( const char *dummy) {
  lstest_main();
  return 0;
}



int md2cmds_run_cmd( const char *cmd) {
  int err, i;
  lspmac_motor_t *mp;
  regmatch_t pmatch[16];
  char cp[64];
  
  if( strlen(cmd) > sizeof( cp)-1) {
    lslogging_log_message( "md2cmds_run_cmd: command too long '%s'", cmd);
    return 1;
  }
  
  err = regexec( &md2cmds_cmd_regex, cmd, 16, pmatch, 0);
  if( err) {
    lslogging_log_message( "md2cmds_run_cmd: no match found from '%s'", cmd);
    return 1;
  }

  for( i=0; i<16; i++) {
    if( pmatch[i].rm_so == -1)
      continue;
    lslogging_log_message( "md2cmds_run_cmd: %d '%.*s'", i, pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
  }

  //
  // get motor name
  //
  snprintf( cp, sizeof( cp)-1, "%.*s", pmatch[4].rm_eo - pmatch[4].rm_so, cmd+pmatch[4].rm_so);
  cp[sizeof( cp)-1] = 0;

  mp = lspmac_find_motor_by_name( cp);
  if( mp == NULL) {
    lslogging_log_message( "md2cmds_run_cmd: could not find motor '%s'", cp);
    return 1;
  }

  if( pmatch[5].rm_so != -1) {
    if( strncmp( cmd+pmatch[5].rm_so, "home", pmatch[5].rm_eo-pmatch[5].rm_so)==0) {
      lslogging_log_message( "md2cmds_run_cmd: homing motor '%s'", cp);
      lspmac_home1_queue( mp);
    } else if( strncmp( cmd+pmatch[5].rm_so, "stop", pmatch[5].rm_eo-pmatch[5].rm_so)==0) {
      lslogging_log_message( "md2cmds_run_cmd: stopping motor '%s'", cp);
      lspmac_abort();
    } else if( strncmp( cmd+pmatch[5].rm_so, "spin", pmatch[5].rm_eo-pmatch[5].rm_so)==0) {
      lslogging_log_message( "md2cmds_run_cmd: spinning motor '%s'", cp);
      lspmac_spin( mp);
    }
  }
  return 0;
}

int md2cmds_settransferpoint( const char *cmd) {
  double ax, ay, az, cx, cy;

  md2cmds_home_prep();

  //
  // Home Kappa
  //
  lspmac_home1_queue( kappa);

  //
  // Home omega
  //
  lspmac_home1_queue( omega);

  if( md2cmds_home_wait( 30.0)) {
    lslogging_log_message( "md2cmds_settransferpoint: homing routines taking too long.  Aborting transfer.");
    lsevents_send_event( "Settransferpoint Aborted");
    return 1;
  }

  md2cmds_home_prep();
  //
  // Home phi (whatever that means)
  //
  lspmac_home1_queue( phi);

  //
  // Wait for the homing routines to finish
  //
  if( md2cmds_home_wait( 30.0)) {
    lslogging_log_message( "md2cmds_settransferpoint: homing routines taking too long.  Aborting transfer.");
    lsevents_send_event( "Settransferpoint Aborted");
    return 1;
  }

  //
  // get positions we'll be needed to report to postgres
  //
  ax = lspmac_getPosition(alignx);
  ay = lspmac_getPosition(aligny);
  az = lspmac_getPosition(alignz);
  cx = lspmac_getPosition(cenx);
  cy = lspmac_getPosition(ceny);

  lspg_query_push( NULL, NULL, "SELECT px.settransferpoint( %0.3f, %0.3f, %0.3f, %0.3f, %0.3f)", ax, ay, az, cx, cy);

  lsevents_send_event( "Settransferpoint Done");
  return 0;
}

/** set the "Back_Vector" preset as the difference between the current position and the Beam position
 *
*/
int md2cmds_setbackvector( const char *cmd) {
  double ax, ay, az;	// current positions
  double axb, ayb, azb;	// preset Beam positions
  double dx, dy, dz;    // the vector components

  //
  // get the current position
  //
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);


  if( !lsredis_find_preset( "align.x", "Beam", &axb) ||
      !lsredis_find_preset( "align.y", "Beam", &ayb) ||
      !lsredis_find_preset( "align.z", "Beam", &azb)) {
    lslogging_log_message( "Could not find preset 'Beam' for at least one of the alignment stage motors");
    lsredis_sendStatusReport( 1, "Beam preset missing.  Go to center moded and center the crystal.");
    return 1;
  }
  
  dx = ax - axb;
  dy = ay - ayb;
  dz = az - azb;

  lsredis_set_preset( "align.x", "Back_Vector", dx);
  lsredis_set_preset( "align.y", "Back_Vector", dy);
  lsredis_set_preset( "align.z", "Back_Vector", dz);
  
  lsredis_set_preset( "align.x", "Back", ax);
  lsredis_set_preset( "align.y", "Back", ay);
  lsredis_set_preset( "align.z", "Back", az);
  
  lsredis_sendStatusReport( 0, "Back_Vector and Back presets set");
  return 0;
}

int md2cmds_set( const char *cmd) {
  int err, i;
  lspmac_motor_t *mp;
  regmatch_t pmatch[16];
  char motor_name[64];
  char preset_name[64];
  char cp[64];
  double rp;
  
  if( strlen(cmd) > sizeof( cp)-1) {
    lslogging_log_message( "md2cmds_set: command too long '%s'", cmd);
    return 1;
  }

  lslogging_log_message( "md2cmds_set: recieved '%s'", cmd);
  


  err = regexec( &md2cmds_cmd_regex, cmd, 16, pmatch, 0);
  if( err) {
    lslogging_log_message( "md2cmds_set: no match found from '%s'", cmd);
    return 1;
  }

  for( i=0; i<16; i++) {
    if( pmatch[i].rm_so == -1)
      continue;
    lslogging_log_message( "md2cmds_run_cmd: %d '%.*s'", i, pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
  }

  //
  // get motor name
  //
  snprintf( motor_name, sizeof( motor_name)-1, "%.*s", pmatch[4].rm_eo - pmatch[4].rm_so, cmd+pmatch[4].rm_so);
  motor_name[sizeof( motor_name)-1] = 0;

  mp = lspmac_find_motor_by_name( motor_name);
  if( mp == NULL) {
    lslogging_log_message( "md2cmds_set: could not find motor '%s'", cp);
    return 1;
  }
  rp = lsredis_getd( mp->redis_position);


  //
  // get preset name
  //
  snprintf( preset_name, sizeof( preset_name)-1, "%.*s", pmatch[5].rm_eo - pmatch[5].rm_so, cmd+pmatch[5].rm_so);
  preset_name[sizeof(preset_name)-1] = 0;


  lsredis_set_preset( motor_name, preset_name, rp);

  return 0;
}


/** Our worker thread
 */
void *md2cmds_worker(
		     void *dummy		/**> [in] Unused but required by protocol		*/
		     ) {

  ENTRY hsearcher, *hrtnval;
  char theCmd[32], *sp;
  int i, err;
  md2cmds_cmd_kv_t *cmdp;

  pthread_mutex_lock( &md2cmds_mutex);

  while( 1) {
    //
    // wait for someone to give us a command (and tell us they did so)
    //
    while( md2cmds_cmd[0] == 0)
      pthread_cond_wait( &md2cmds_cond, &md2cmds_mutex);


    //
    // pull out the command name itself from the string we were given
    //
    for( i=0, sp=md2cmds_cmd; i<sizeof( theCmd)-1; i++, sp++) {
      if( *sp == 0 || *sp == ' ') {
	theCmd[i] = 0;
	break;
      }
      theCmd[i] = *sp;
    }
    theCmd[sizeof(theCmd)-1]=0;

    hsearcher.key  = theCmd;
    hsearcher.data = NULL;

    errno = 0;
    err = hsearch_r( hsearcher, FIND, &hrtnval, &md2cmds_hmap);
    if( err == 0) {
      lslogging_log_message( "md2cmds_worker: hsearch_r failed.  theCmd = '%s' from string '%s'", theCmd, md2cmds_cmd);
      md2cmds_cmd[0] = 0;
      continue;
    }
    lslogging_log_message( "md2cmds_worker: Found command '%s'", theCmd);
    if( hrtnval != NULL) {
      cmdp = (md2cmds_cmd_kv_t *)hrtnval;
      err = cmdp->v( md2cmds_cmd);
      if( err) {
	lslogging_log_message( "md2cmds_worker: Command failed: '%s'", md2cmds_cmd);
	//
	// At this point we'd clear the queue but the queue is currently too short to bother doing that
	//
      }
    }

    md2cmds_cmd[0] = 0;
  }
}

void md2cmds_coordsys_1_stopped_cb( char *event) {
}
void md2cmds_coordsys_2_stopped_cb( char *event) {
}
void md2cmds_coordsys_3_stopped_cb( char *event) {
}
void md2cmds_coordsys_4_stopped_cb( char *event) {
}
void md2cmds_coordsys_5_stopped_cb( char *event) {
}
void md2cmds_coordsys_7_stopped_cb( char *event) {
}


static int ca_last_enabled = 1;
void md2cmds_disable_ca_last_cb( char *event) {
  ca_last_enabled = 0;
}

void md2cmds_enable_ca_last_cb( char *event) {
  double scint_target;

  pthread_mutex_lock( &scint->mutex);
  scint_target = scint->requested_position;
  pthread_mutex_unlock( &scint->mutex);

  if( scint_target < 10.0) {
    ca_last_enabled = 1;
  }
}

/** Save centering and alignment stage positions
 *  to restore on restart
 */
void md2cmds_ca_last_cb( char *event) {
  char *phase;
  char motor_name[64];
  int i;
  lspmac_motor_t *mp;

  //
  // don't save the current position if the scintilator is out
  //
  if( !ca_last_enabled)
    return;

  phase = lsredis_getstr( lsredis_get_obj( "phase"));
  if( strcmp( phase, "center") == 0 || strcmp( phase, "dataCollection") == 0) {
    motor_name[0] = 0;
    for( i=0; i<sizeof(motor_name)-1; i++) {
      if( event[i] == ' ')
	break;
      motor_name[i] = event[i];
      motor_name[i+1] = 0;
    }
    if( i < sizeof(motor_name)) {
      mp = lspmac_find_motor_by_name( motor_name);
      if( mp != NULL ) {
	lsredis_set_preset( motor_name, "Beam", lspmac_getPosition( mp));
      }
    }
  }
  free( phase);
}


/** restore previous md2 phase
 */
void md2cmds_lspmac_ready_cb( char *event) {
  char *phase;
  char s[32];

  phase = lsredis_getstr( lsredis_get_obj( "phase"));
  if( phase && phase[0] != 0) {
    snprintf( s, sizeof(s)-1, "changeMode %s", phase);
    s[sizeof(s)-1] = 0;
    md2cmds_phase_change( s);
  }
  if( phase)
    free( phase);
}

/** request that whatever md2cmds command is running that it stop and clean up
 */
void md2cmds_lspmac_abort_cb( char *event) {
  pthread_mutex_lock( &abort_requested_mutex);
  abort_requested = 1;
  pthread_mutex_unlock( &abort_requested_mutex);
}

void md2cmds_quitting_cb( char *event) {
  lsredis_sendStatusReport( 1, "MD2 Program Stopped");
}


/** Initialize the md2cmds module
 */
void md2cmds_init() {
  ENTRY hloader, *hrtnval;
  int i, err;
  int ncmds;

  pthread_mutexattr_t mutex_initializer;

  pthread_mutexattr_init( &mutex_initializer);
  pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);

  pthread_mutex_init( &md2cmds_mutex, &mutex_initializer);
  pthread_cond_init( &md2cmds_cond, NULL);


  pthread_mutex_init( &md2cmds_moving_mutex, &mutex_initializer);
  pthread_cond_init(  &md2cmds_moving_cond, NULL);

  pthread_mutex_init( &md2cmds_homing_mutex, &mutex_initializer);
  pthread_cond_init(  &md2cmds_homing_cond, NULL);


  err = regcomp( &md2cmds_cmd_regex, " *([^ ]+) (([^ ]+)\\.presets\\..)*([^ ]*) *([^ ]*)", REG_EXTENDED);
  if( err != 0) {
    int nerrmsg;
    char *errmsg;

    nerrmsg = regerror( err, &md2cmds_cmd_regex, NULL, 0);
    if( nerrmsg > 0) {
      errmsg = calloc( nerrmsg, sizeof( char));
      nerrmsg = regerror( err, &md2cmds_cmd_regex, errmsg, nerrmsg);
      lslogging_log_message( "md2cmds_init: %s", errmsg);
      free( errmsg);
    }
  }

  md2cmds_md_status_code = lsredis_get_obj( "md2_status_code");
  lsredis_setstr( md2cmds_md_status_code, "7");

  ncmds = sizeof(md2cmds_cmd_kvs)/sizeof(md2cmds_cmd_kvs[0]);
  if( pgpmac_use_pg) {
    ncmds += sizeof(md2cmds_cmd_pg_kvs)/sizeof(md2cmds_cmd_pg_kvs[0]);
  }

  hcreate_r( 2 * ncmds, &md2cmds_hmap);

  for( i=0; i<sizeof(md2cmds_cmd_kvs)/sizeof(md2cmds_cmd_kvs[0]); i++) {
    hloader.key  = md2cmds_cmd_kvs[i].k;
    hloader.data = md2cmds_cmd_kvs[i].v;
    err = hsearch_r( hloader, ENTER, &hrtnval, &md2cmds_hmap);
    if( err == 0) {
      lslogging_log_message( "md2cmds_init: hsearch_r returned an error for item %d: %s", i, strerror( errno));
    }
  }

  if( pgpmac_use_pg) {
    for( i=0; i<sizeof(md2cmds_cmd_pg_kvs)/sizeof(md2cmds_cmd_pg_kvs[0]); i++) {
      hloader.key  = md2cmds_cmd_pg_kvs[i].k;
      hloader.data = md2cmds_cmd_pg_kvs[i].v;
      err = hsearch_r( hloader, ENTER, &hrtnval, &md2cmds_hmap);
      if( err == 0) {
	lslogging_log_message( "md2cmds_init: hsearch_r returned an error for item %d: %s", i, strerror( errno));
      }
    }
  }
}

void md2cmds_redis_abort_cb() {
  lsredis_obj_t *p;

  p = lsredis_get_obj( "md2cmds.abort");
  
  lslogging_log_message( "md2cmds_redis_abort_cb: arrived with value %d", lsredis_getb(p));

  if( lsredis_getb( p)) {
    lsevents_send_event( "Abort Requested");
    lsredis_setstr( p, "0");
  }
}



/** Start up the thread
 */
pthread_t *md2cmds_run() {
  lsredis_set_onSet( lsredis_get_obj( "md2cmds.abort"), md2cmds_redis_abort_cb);
  
  pthread_create( &md2cmds_thread, NULL,                md2cmds_worker, NULL);
  lsevents_add_listener( "^omega crossed zero$",        md2cmds_rotate_cb);
  lsevents_add_listener( "^omega In Position$",         md2cmds_maybe_rotate_done_cb);
  lsevents_add_listener( ".+ (Moving|In Position)$",    md2cmds_maybe_done_moving_cb);
  lsevents_add_listener( "(.+) (Homing|Homed)$",        md2cmds_maybe_done_homing_cb);
  lsevents_add_listener( "^capz (Moving|In Position)$", md2cmds_time_capz_cb);
  lsevents_add_listener( "^Coordsys 1 Stopped$",        md2cmds_coordsys_1_stopped_cb);
  lsevents_add_listener( "^Coordsys 2 Stopped$",        md2cmds_coordsys_2_stopped_cb);
  lsevents_add_listener( "^Coordsys 3 Stopped$",        md2cmds_coordsys_3_stopped_cb);
  lsevents_add_listener( "^Coordsys 4 Stopped$",        md2cmds_coordsys_4_stopped_cb);
  lsevents_add_listener( "^Coordsys 5 Stopped$",        md2cmds_coordsys_5_stopped_cb);
  lsevents_add_listener( "^Coordsys 7 Stopped$",        md2cmds_coordsys_7_stopped_cb);
  lsevents_add_listener( "^cam.zoom Moving$",	        md2cmds_set_scale_cb);
  lsevents_add_listener( "^(align\\.(x|y|z)|centering.(x|y)) In Position$", md2cmds_ca_last_cb);
  lsevents_add_listener( "^scint Moving$",              md2cmds_disable_ca_last_cb);
  lsevents_add_listener( "^scint In Position$",         md2cmds_enable_ca_last_cb);
  lsevents_add_listener( "^LSPMAC Done Initializing$",  md2cmds_lspmac_ready_cb);
  lsevents_add_listener( "^Abort Requested$",           md2cmds_lspmac_abort_cb);
  lsevents_add_listener( "^Quitting Program$",          md2cmds_quitting_cb);

  lsredis_sendStatusReport( 0, "MD2 Started");


  return &md2cmds_thread;
}
