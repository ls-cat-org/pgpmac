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
 Command                                     Meaning

 abort                                       Stop the current motion and put the system into a known state

 changeMode  <mode>                          Where <mode> is one of "manualMount", "robotMount", "center", "dataCollection", "beamLocation", "safe"

 collect                                     Start collecting data

 homestages                                  Home centering stages and alignment stages

 moveAbs  <motor> <position_or_presetName>   Move the given motor to the said position.  Common preset names are "In", "Out", "Cover".

 moveRel  <motor> <relative_position>        Move the given motor by the relative amount from its current position

 nonrotate                                   Used for local centering when we do not want to trigger movie making

 raster <key>                                Trigger raster scan for key

 rotate                                      Used for remote centering where we do want to make a movie

 run <motor> <command>                       Run a special command on <motor> where <command> is one of "home", "spin", "stop"

 shutterless                                 Like collect but only used for line segment mode with the Eiger

 set <motor1> [<motor2>...<motorN>] <preset> Set all named motors current position as <preset>.  <preset> will be created if it does not currently exist.

 settransferpoint                            Set the current motor positions at the alignment point for robot transfers.

 setbackvector                               Set the current alignment stage position as the Back preset and the difference between Back and Beam as Back_Vector

 setbeamstoplimits                           Set P6000 and P6001 for the pmac plcc0 check of the beamstop position.  Sets current position +/- 100 microns

 setsamplebeam                               Set the current alignment and centering positions as the the Beam preset for the respective stages

 test                                        Run unit tests (which are not very complete at this point)

 transfer                                    Transfer the next transfer

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


pthread_cond_t  md2cmds_cond;           //!< condition to signal when it's time to run an md2 command
pthread_mutex_t md2cmds_mutex;          //!< mutex for the condition

int md2cmds_moving_queue_wait = 0;      //! wait for command to have been dequeued and run
pthread_cond_t  md2cmds_moving_cond;    //!< coordinate call and response
pthread_mutex_t md2cmds_moving_mutex;   //!< message passing between md2cmds and pg

int md2cmds_homing_count = 0;           //!< We've asked a motor to home
pthread_cond_t md2cmds_homing_cond;     //!< coordinate homing and homed
pthread_mutex_t md2cmds_homing_mutex;   //!< our mutex;

int md2cmds_shutter_open_flag = 0;      //!< Our own shutter open flag (may not work for very short open times
pthread_cond_t  md2cmds_shutter_cond;
pthread_mutex_t md2cmds_shutter_mutex;

int md2cmds_moving_count = 0;

char md2cmds_cmd[MD2CMDS_CMD_LENGTH];   //!< our command;

lsredis_obj_t *md2cmds_md_status_code;

static pthread_t md2cmds_thread;

static int rotating = 0;                //!< flag: when omega is in position after a rotate we want to re-home omega

static int abort_requested = 0;         //!< flag: indicates an out of band abort request
static pthread_mutex_t abort_requested_mutex;

static double md2cmds_capz_moving_time = NAN;

static struct hsearch_data md2cmds_hmap;

static regex_t md2cmds_cmd_regex;

static regex_t md2cmds_cmd_set_regex;

typedef struct md2cmds_cmd_kv_struct {
  char *k;
  int (*v)( const char *);
} md2cmds_cmd_kv_t;

void md2cmds_mvcenter_move(double, double, double, double, double);


int md2cmds_abort(            const char *);
int md2cmds_collect(          const char *);
int md2cmds_goto_point(       const char *);
int md2cmds_homestages(       const char *);
int md2cmds_moveAbs(          const char *);
int md2cmds_moveRel(          const char *);
int md2cmds_phase_change(     const char *);
int md2cmds_preSet(           const char *);
int md2cmds_raster(           const char *);
int md2cmds_run_cmd(          const char *);
int md2cmds_rotate(           const char *);
int md2cmds_nonrotate(        const char *);
int md2cmds_shutterless(      const char *);
int md2cmds_set(              const char *);
int md2cmds_setbeamstoplimits(const char *);
int md2cmds_settransferpoint( const char *);
int md2cmds_setbackvector(    const char *);
int md2cmds_setsamplebeam(    const char *);
int md2cmds_test(             const char *);
int md2cmds_transfer(         const char *);

//
// Commands that do not rely on lspg calls
//
static md2cmds_cmd_kv_t md2cmds_cmd_kvs[] = {
  { "abort",            md2cmds_abort},
  { "changeMode",       md2cmds_phase_change},
  { "gotoPoint",        md2cmds_goto_point},
  { "homestages",       md2cmds_homestages},
  { "moveAbs",          md2cmds_moveAbs},
  { "moveRel",          md2cmds_moveRel},
  { "preSet",           md2cmds_preSet},
  { "raster",           md2cmds_raster},
  { "run",              md2cmds_run_cmd},
  { "test",             md2cmds_test},
  { "set",              md2cmds_set},
  { "setbackvector",    md2cmds_setbackvector},
  { "setbeamstoplimits",md2cmds_setbeamstoplimits},
  { "setsamplebeam",    md2cmds_setsamplebeam}
};

//
// Commands that do rely on lspg calls
//
static md2cmds_cmd_kv_t md2cmds_cmd_pg_kvs[] = {
  { "collect",          md2cmds_collect},
  { "nonrotate",        md2cmds_nonrotate},
  { "rotate",           md2cmds_rotate},
  { "shutterless",      md2cmds_shutterless},
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


void md2cmds_motion_reset_cb( char *evt) {
  lsredis_setstr( md2cmds_md_status_code, "%s", "3");
  pthread_mutex_lock( &md2cmds_moving_mutex);
  md2cmds_moving_count = 0;
  pthread_cond_signal( &md2cmds_moving_cond);
  pthread_mutex_unlock( &md2cmds_moving_mutex);
}

void md2cmds_motion_reset() {
  lspmac_abort();
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
  //                  Q40     = X Value
  //                  Q41     = Y Value
  //                  Q42     = Z Value
  //                  Q43     = U Value
  //                  Q44     = V Value
  //                  Q45     = W Value
  //
  
  lspmac_SockSendDPline( "organs", "&5 Q40=0 Q41=%d Q42=%d Q43=%d Q44=%d Q45=%d Q100=16 B170R", cay, caz, ccy, ccz, csz);
}


int md2cmds_robotMount_start( double *move_time, int *mmask) {
  *mmask = 0;
  int err;
  double kappa_home_time;
  
  kappa_home_time = lsredis_getd( lsredis_get_obj("kappa.home_time"));

  md2cmds_home_prep();

  //
  // Move 'em
  //

  lspmac_home1_queue( kappa);
  lspmac_home1_queue( omega);

  mmask = 0;
  err = lspmac_est_move_time( move_time, mmask,
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

  *move_time = *move_time > kappa_home_time ? *move_time : kappa_home_time;

  return err;
}

int md2cmds_robotMount_finish( double move_time, int mmask) {
  int err;

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

  lsevents_send_event( "Mode robotMount Done");
  
  return err;
}

/** Transfer a sample
 *  \param dummy Unused
 */
int md2cmds_transfer( const char *dummy) {
  int nextsample, abort_now;
  double ax, ay, az, cx, cy, horz, vert, oref;
  int mmask, err;
  double move_time;
  int transferAborted;

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

  lsredis_sendStatusReport( 0, "Moving MD2 devices to sample transfer position");
  //
  // Go to sample transfer mode
  //
  lsredis_sendStatusReport( 0, "Preparing MD2 for robotic mounting");
  if( md2cmds_robotMount_start( &move_time, &mmask)) {
    lsredis_sendStatusReport( 1, "Failed to start motors");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }  

  // Out of band transfer abort implemented via Abort Requested event

  //
  // get positions we'll be needed to report to postgres
  //
  ax = lspmac_getPosition(alignx);
  ay = lspmac_getPosition(aligny);
  az = lspmac_getPosition(alignz);
  cx = lspmac_getPosition(cenx);
  cy = lspmac_getPosition(ceny);
  oref = lsredis_getd(lsredis_get_obj( "omega.reference")) * M_PI/180.;

  horz = -( cx * cos(oref) - cy * sin(oref));
  vert =    cx * sin(oref) + cy * cos(oref);

  lspg_starttransfer_call( nextsample, 0, ax, ay, az, horz, vert, move_time);

  lspg_starttransfer_wait();
  if( lspg_starttransfer.query_error) {
    lsredis_sendStatusReport( 1, "A database related error occurred trying to start the transfer.  This is neither normal nor OK.  Aborting transfer.");
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

  if( md2cmds_robotMount_finish( move_time, mmask)) {
    lsredis_sendStatusReport( 1, "Failed to put MD2 in transfer mode");
    lslogging_log_message( "md2cmds_transfer: tired of waiting for motors.  Tried for %d seconds.", move_time);
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
  
  // refuse to go on if we do not have positive confirmation that the
  // fluorescence detector is back (TODO: how about all those organs?)
  //
  if( lspmac_getBIPosition( fluor_back) != 1) {
    lsredis_sendStatusReport( 1, "The fluorescence detector is possibly stuck in the wrong position.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: It looks like the fluorescence dectector is not back");
    lsevents_send_event( "Transfer Aborted");
    return 1;
  }

  // refuse to go on if we do not have positive confirmation that the backlight is down 
  //
  if( lspmac_getBIPosition( blight_down) != 1) {
    lsredis_sendStatusReport( 1, "The backlight is possibly stuck in the wrong position.  Aborting transfer.");
    lslogging_log_message( "md2cmds_transfer: It looks like the back light is not down");
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

  transferAborted = 0;
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
    transferAborted = 1;
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

  if( transferAborted)
    return transferAborted;

  if( nextsample != 0) {
    md2cmds_phase_change( "changeMode fastCentering");
  }

  lspmac_moveabs_wait( cryo, 2.0);

  lsredis_sendStatusReport( 0, "Transfer completed.");
  lsevents_send_event( "Transfer Done");

  return 0;
}


/** Move a motor to the position requested
 *  Returns non zero on error
 */
int md2cmds_moveAbs(
                    const char *cmd                     /**< [in] The full command string to parse, ie, "moveAbs omega 180"     */
                    ) {
  static const char *id = "md2cmds_moveAbs";
  double fpos;
  char *endptr;
  lspmac_motor_t *mp;
  int err, prec;
  double move_time;
  regmatch_t pmatch[32];
  char motor_name[64];
  char preset_name[64];
  char position_string[64];
  int preset_index;
  int i;
  double rp;

  // ignore nothing
  if( cmd == NULL || *cmd == 0) {
    return 1;
  }

  //
  // "parse" our command line
  //
  err = regexec( &md2cmds_cmd_set_regex, cmd, 32, pmatch, REG_EXTENDED);
  if( err) {
    lslogging_log_message( "%s: no match found from '%s'", id, cmd);
    return 1;
  }

  // entire string is match index 0
  //   1         2        3               N+1      N+2         N+3
  // moveAbs <motor1> <position1>[ ... <motorN> <positionN>] [preset]
  
  //
  //  Allow multiple motor/position pairs
  //
  // TODO: move them all at the same time
  //
  
  // Look for an even numbered match (M) that does not have an odd
  // numbered match (M+1).  That's our preset.  If there is no such
  // match then there is no preset.  We'll start by trying to find the
  // last blank even numbered argument.  The smallest index that can
  // have a preset name is 4 (a single motor name followed by a single
  // position followed by a prefix name).  Hence, we start our search
  // at index 6.  If it's blank AND index 5 is blank then index 4 is
  // our preset name. (and so forth counting by twos).
  //
  preset_index = -1;
  for(i=4; i<30; i+=2) {
    if (pmatch[i+2].rm_so == -1 || (pmatch[i+2].rm_eo == pmatch[i+2].rm_so)) {
      if ((pmatch[i+1].rm_so == -1 || (pmatch[i+1].rm_eo == pmatch[i+1].rm_so))) {
        if ((pmatch[i].rm_so != -1 && (pmatch[i].rm_eo > pmatch[i].rm_so))) {
          preset_index = i;
        }
      }
      break;
    }
  }

  //
  // maybe get preset name
  //
  if (preset_index > -1) {
    snprintf(preset_name, sizeof(preset_name)-1, "%.*s", pmatch[preset_index].rm_eo - pmatch[preset_index].rm_so, cmd+pmatch[preset_index].rm_so);
    preset_name[sizeof(preset_name)-1] = 0;
    lslogging_log_message("%s: found preset set request for preset %s  index %d  eo %d  so %d", id, preset_name, preset_index, pmatch[preset_index].rm_eo, pmatch[preset_index].rm_so);
  }
  
  //
  // Loop over motors
  //
  for (i=2; i<31; i+=2) {
    if (i == preset_index) {
      // end of line when preset is defined
      break;
    }

    if (pmatch[i].rm_so == -1 || (pmatch[i].rm_eo == pmatch[i].rm_so)) {
      // end of line when no preset is defined
      break;
    }

    //
    // Get our motor name
    //
    snprintf(motor_name, sizeof(motor_name)-1, "%.*s", pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
    motor_name[sizeof(motor_name)-1] = 0;
    lslogging_log_message("%s: motor name: %s", id, motor_name);

    mp = lspmac_find_motor_by_name( motor_name);
    if( mp == NULL) {
      lslogging_log_message( "%s: cannot find motor %s", id, motor_name);
      lsredis_sendStatusReport( 1, "moveAbs can't find motor named %s", motor_name);
      err = 1;
      break;
    }

    //
    // Get our position as a string (cause it might be a prefix name
    // or might be a floating point number.
    //
    snprintf(position_string, sizeof(position_string)-1, "%.*s", pmatch[i+1].rm_eo - pmatch[i+1].rm_so, cmd+pmatch[i+1].rm_so);
    position_string[sizeof(position_string)-1] = 0;

    fpos = strtod(position_string, &endptr);
    if( endptr == position_string) {
      //
      // Maybe we have a preset.  Give it a whirl
      //
      lsredis_sendStatusReport( 0, "moving  %s to '%s'", motor_name, position_string);
      
      err = lspmac_est_move_time( &move_time, NULL,
                                  mp, 1, position_string, 0.0,
                                  NULL);
      
      if( err) {
        lsredis_sendStatusReport( 1, "Failed to move motor %s to '%s'", mp->name, position_string);
        break;
      }
      
      err = lspmac_est_move_time_wait( move_time + 10.0, 0, mp, NULL);
      
      if( err) {
        lsredis_sendStatusReport( 1, "Timed out waiting %.1f seconds for motor %s to finish moving", move_time+10.0, mp->name);
        //
        // error.  Too bad.
        break;
      }
      lsredis_sendStatusReport( 0, "%s", "");
      //
      // go to the next motor
      continue;
    }

    //
    // Here we have a numeric position to move the motor to
    //
    prec = lsredis_getl( mp->printPrecision);
    if( mp != NULL && mp->moveAbs != NULL) {
      pgpmac_printf( "Moving %s to %f\n", motor_name, fpos);
      lsredis_sendStatusReport( 0, "moving  %s to %.*f", motor_name, prec, fpos);
      err = lspmac_est_move_time( &move_time, NULL,
                                  mp, 1, NULL, fpos,
                                  NULL);
      
      if( err) {
        lsredis_sendStatusReport( 1, "Failed to move motor %s to '%.3f'", mp->name, fpos);
        break;
      }
      
      err = lspmac_est_move_time_wait( move_time + 10.0, 0, mp, NULL);
      if( err) {
        lsredis_sendStatusReport( 1, "Timed out waiting %.1f seconds for motor %s to finish moving", move_time+10.0, mp->name);
      } else {
        lsredis_sendStatusReport( 0, "%s", "");
      }
    }
  }

  if (err == 0 && preset_index > -1) {
    //
    // OK, we have no errors AND we have a preset
    //
    // We assume that if any motor did not make it to its requested
    // position then we do not want any of the presets set for any of
    // the motors.  Hence, we travel through the motor list again and
    // set the presets one by one.
    //
    for (i=2; i<31; i+=2) {
      if (i == preset_index) {
        // end of line when preset is defined
        break;
      }
      
      if (pmatch[i].rm_so == -1 || (pmatch[i].rm_eo == pmatch[i].rm_so)) {
        // end of line when no preset is defined
        break;
      }
      
      //
      // Get our motor name
      //
      snprintf(motor_name, sizeof(motor_name)-1, "%.*s", pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
      motor_name[sizeof(motor_name)-1] = 0;
      lslogging_log_message("%s: motor name: %s", id, motor_name);
      mp = lspmac_find_motor_by_name(motor_name);
      //
      // Not likely the motor would be found previously but not found
      // now.  Hence, no error check for NULL mp here.
      //
      
      rp = lsredis_getd( mp->redis_position);
      lsredis_set_preset( motor_name, preset_name, rp);
      lslogging_log_message("%s: set preset %s for motor %s to position %f", id, preset_name, motor_name, rp);

    }
  }
  return err;
}

/** 
 *  Set a named preset to the requested position without first moving the motor there
 */
int md2cmds_preSet(
                    const char *cmd                     /**< [in] The full command string to parse, ie, "moveAbs omega 180"     */
                    ) {
  static const char *id = "md2cmds_preSet";
  double fpos;
  char *endptr;
  lspmac_motor_t *mp;
  int err;
  regmatch_t pmatch[32];
  char motor_name[64];
  char preset_name[64];
  char position_string[64];
  int preset_index;
  int i;

  // ignore nothing
  if( cmd == NULL || *cmd == 0) {
    return 1;
  }

  //
  // "parse" our command line
  //
  err = regexec( &md2cmds_cmd_set_regex, cmd, 32, pmatch, REG_EXTENDED);
  if( err) {
    lslogging_log_message( "%s: no match found from '%s'", id, cmd);
    return 1;
  }

  // entire string is match index 0
  //   1         2        3               N+1      N+2         N+3
  // moveAbs <motor1> <position1>[ ... <motorN> <positionN>] [preset]
  
  //
  //  Allow multiple motor/position pairs
  //
  // TODO: move them all at the same time
  //
  
  // Look for an even numbered match (M) that does not have an odd
  // numbered match (M+1).  That's our preset.  If there is no such
  // match then there is no preset.  We'll start by trying to find the
  // last blank even numbered argument.  The smallest index that can
  // have a preset name is 4 (a single motor name followed by a single
  // position followed by a prefix name).  Hence, we start our search
  // at index 6.  If it's blank AND index 5 is blank then index 4 is
  // our preset name. (and so forth counting by twos).
  //
  preset_index = -1;
  for(i=4; i<30; i+=2) {
    if (pmatch[i+2].rm_so == -1 || (pmatch[i+2].rm_eo == pmatch[i+2].rm_so)) {
      if ((pmatch[i+1].rm_so == -1 || (pmatch[i+1].rm_eo == pmatch[i+1].rm_so))) {
        if ((pmatch[i].rm_so != -1 && (pmatch[i].rm_eo > pmatch[i].rm_so))) {
          preset_index = i;
        }
      }
      break;
    }
  }

  if (preset_index == -1) {
    lslogging_log_message("%s: no preset found at the end of the command string '%s'", id, cmd);
    return 1;
  }


  //
  // get preset name
  //
  snprintf(preset_name, sizeof(preset_name)-1, "%.*s", pmatch[preset_index].rm_eo - pmatch[preset_index].rm_so, cmd+pmatch[preset_index].rm_so);
  preset_name[sizeof(preset_name)-1] = 0;
  lslogging_log_message("%s: found preset set request for preset %s  index %d  eo %d  so %d", id, preset_name, preset_index, pmatch[preset_index].rm_eo, pmatch[preset_index].rm_so);

  
  //
  // Loop over motors
  //
  for (i=2; i<31; i+=2) {
    if (i == preset_index) {
      // end of line when preset is defined
      break;
    }

    if (pmatch[i].rm_so == -1 || (pmatch[i].rm_eo == pmatch[i].rm_so)) {
      // end of line when no preset is defined
      break;
    }

    //
    // Get our motor name
    //
    snprintf(motor_name, sizeof(motor_name)-1, "%.*s", pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
    motor_name[sizeof(motor_name)-1] = 0;
    lslogging_log_message("%s: motor name: %s", id, motor_name);

    mp = lspmac_find_motor_by_name( motor_name);
    if( mp == NULL) {
      lslogging_log_message( "%s: cannot find motor %s", id, motor_name);
      lsredis_sendStatusReport( 1, "moveAbs can't find motor named %s", motor_name);
      err = 1;
      break;
    }

    //
    // Get our position as a string (cause it might be a prefix name
    // or might be a floating point number.
    //
    snprintf(position_string, sizeof(position_string)-1, "%.*s", pmatch[i+1].rm_eo - pmatch[i+1].rm_so, cmd+pmatch[i+1].rm_so);
    position_string[sizeof(position_string)-1] = 0;

    fpos = strtod(position_string, &endptr);
    if( endptr == position_string) {
      //
      // We didn't find a number, perhaps we have a named preset
      //

      if (strcmp(position_string, "current") == 0) {
        // Special prefix named 'current' means just to use the motor's current position
        mp = lspmac_find_motor_by_name(motor_name);
        if (mp == NULL) {
          lslogging_log_message("%s: could not find a motor named '%s'", id, motor_name);
          continue;
        }
        fpos = lsredis_getd(mp->redis_position);
      } else {
        // look up the normal prefix
        err = lsredis_find_preset(motor_name, position_string, &fpos);
        if (err == 0) {
          lslogging_log_message("%s: could not find preset named '%s' for motor %s", id, position_string, motor_name);
          continue;
        }
      }
    }

    // by hook or by crook we now have fpos containing the position
    // that we'd like to call prefix_name

    lsredis_set_preset(motor_name, preset_name, fpos);
  }
  return err;
}

/** Move a motor to the position requested
 *  Returns non zero on error
 */
int md2cmds_moveRel(
                    const char *cmd                     /**< [in] The full command string to parse, ie, "moveAbs omega 180"     */
                    ) {

  static const char *id = "md2cmds_moveRel";
  double fpos;
  char *endptr;
  lspmac_motor_t *mp;
  int err, prec;
  double move_time;
  regmatch_t pmatch[32];
  char motor_name[64];
  char preset_name[64];
  char position_string[64];
  int preset_index;
  int i;
  double rp;

  // ignore nothing
  if( cmd == NULL || *cmd == 0) {
    return 1;
  }

  //
  // "parse" our command line
  //
  err = regexec( &md2cmds_cmd_set_regex, cmd, 32, pmatch, REG_EXTENDED);
  if( err) {
    lslogging_log_message( "%s: no match found from '%s'", id, cmd);
    return 1;
  }

  // entire string is match index 0
  //   1         2       3             N+1     N+2       N+3
  // moveRel <motor1> <delta1>[ ... <motorN> <deltaN>] [preset]
  
  //
  //  Allow multiple motor/position pairs
  //
  // TODO: move them all at the same time
  //
  
  // Look for an even numbered match (M) that does not have an odd
  // numbered match (M+1).  That's our preset.  If there is no such
  // match then there is no preset.  We'll start by trying to find the
  // last blank even numbered argument.  The smallest index that can
  // have a preset name is 4 (a single motor name followed by a single
  // position followed by a prefix name).  Hence, we start our search
  // at index 6.  If it's blank AND index 5 is blank then index 4 is
  // our preset name. (and so forth counting by twos).
  //
  preset_index = -1;
  for(i=4; i<30; i+=2) {
    if (pmatch[i+2].rm_so == -1 || (pmatch[i+2].rm_eo == pmatch[i+2].rm_so)) {
      if ((pmatch[i+1].rm_so == -1 || (pmatch[i+1].rm_eo == pmatch[i+1].rm_so))) {
        if ((pmatch[i].rm_so != -1 && (pmatch[i].rm_eo > pmatch[i].rm_so))) {
          preset_index = i;
        }
      }
      break;
    }
  }

  //
  // maybe get preset name
  //
  if (preset_index > -1) {
    snprintf(preset_name, sizeof(preset_name)-1, "%.*s", pmatch[preset_index].rm_eo - pmatch[preset_index].rm_so, cmd+pmatch[preset_index].rm_so);
    preset_name[sizeof(preset_name)-1] = 0;
    lslogging_log_message("%s: found preset set request for preset %s  index %d  eo %d  so %d", id, preset_name, preset_index, pmatch[preset_index].rm_eo, pmatch[preset_index].rm_so);
  }
  
  //
  // Loop over motors
  //
  for (i=2; i<31; i+=2) {
    if (i == preset_index) {
      // end of line when preset is defined
      break;
    }

    if (pmatch[i].rm_so == -1 || (pmatch[i].rm_eo == pmatch[i].rm_so)) {
      // end of line when no preset is defined
      break;
    }

    //
    // Get our motor name
    //
    snprintf(motor_name, sizeof(motor_name)-1, "%.*s", pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
    motor_name[sizeof(motor_name)-1] = 0;
    lslogging_log_message("%s: motor name: %s", id, motor_name);

    mp = lspmac_find_motor_by_name( motor_name);
    if( mp == NULL) {
      lslogging_log_message( "%s: cannot find motor %s", id, motor_name);
      lsredis_sendStatusReport( 1, "can't find motor named %s", motor_name);
      err = 1;
      break;
    }

    //
    // Get our position as a string (cause it might be a prefix name
    // or might be a floating point number.
    //
    snprintf(position_string, sizeof(position_string)-1, "%.*s", pmatch[i+1].rm_eo - pmatch[i+1].rm_so, cmd+pmatch[i+1].rm_so);
    position_string[sizeof(position_string)-1] = 0;

    fpos = lsredis_getd(mp->redis_position) + strtod(position_string, &endptr);
    if( endptr == position_string) {
      //
      // Maybe we have a preset.  Unfortunately moveRel presets are currently not supported.  Perhaps they should be.
      //
      lslogging_log_message("%s: We have may have a relative move using a preset.  Not currently supported. Sorry.", id);
      continue;
    }

    //
    // Here we have a numeric position to move the motor to
    //
    prec = lsredis_getl( mp->printPrecision);
    if( mp != NULL && mp->moveAbs != NULL) {
      pgpmac_printf( "Moving %s to %f\n", motor_name, fpos);
      lsredis_sendStatusReport( 0, "moving  %s to %.*f", motor_name, prec, fpos);
      err = lspmac_est_move_time( &move_time, NULL,
                                  mp, 1, NULL, fpos,
                                  NULL);
      
      if( err) {
        lsredis_sendStatusReport( 1, "Failed to move motor %s to '%.3f'", mp->name, fpos);
        break;
      }
      
      err = lspmac_est_move_time_wait( move_time + 10.0, 0, mp, NULL);
      if( err) {
        lsredis_sendStatusReport( 1, "Timed out waiting %.1f seconds for motor %s to finish moving", move_time+10.0, mp->name);
      } else {
        lsredis_sendStatusReport( 0, "%s", "");
      }
    }
  }

  if (err == 0 && preset_index > -1) {
    //
    // OK, we have no errors AND we have a preset
    //
    // We assume that if any motor did not make it to its requested
    // position then we do not want any of the presets set for any of
    // the motors.  Hence, we travel through the motor list again and
    // set the presets one by one.
    //
    for (i=2; i<31; i+=2) {
      if (i == preset_index) {
        // end of line when preset is defined
        break;
      }
      
      if (pmatch[i].rm_so == -1 || (pmatch[i].rm_eo == pmatch[i].rm_so)) {
        // end of line when no preset is defined
        break;
      }
      
      //
      // Get our motor name
      //
      snprintf(motor_name, sizeof(motor_name)-1, "%.*s", pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
      motor_name[sizeof(motor_name)-1] = 0;
      lslogging_log_message("%s: motor name: %s", id, motor_name);
      mp = lspmac_find_motor_by_name(motor_name);
      //
      // Not likely the motor would be found previously but not found
      // now.  Hence, no error check for NULL mp here.
      //
      

      rp = lsredis_getd( mp->redis_position);
      lsredis_set_preset( motor_name, preset_name, rp);
      lslogging_log_message("%s: set preset %s for motor %s to position %f", id, preset_name, motor_name, rp);
    }
  }
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
  int mmask;

  lsevents_send_event( "Mode robotMount Starting");
  if( md2cmds_robotMount_start( &move_time, &mmask)) {
    lslogging_log_message( "md2cmds_phase_robotMount: error starting move");
    lsevents_send_event( "Mode robotMount Aborted");
    return 1;
  }

  return md2cmds_robotMount_finish( move_time, mmask);
}

/** Go to fluorescence mode
 */
int md2cmds_phase_fluorescence() {
  double move_time;
  int mmask, err;

  lsevents_send_event( "Mode fluorescence Starting");
  lsredis_sendStatusReport( 0, "Putting MD-2 in Fluorescence mode");

  // Yeah,  Let's move

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
                              apery,      0, "In",    0.0,
                              aperz,      0, "In",    0.0,
                              capy,       0, "In",    0.0,
                              capz,       0, "Out",   0.0,
                              scint,      0, "Cover", 0.0,
                              blight_ud,  1, NULL,    0.0,
                              cryo,       1, NULL,    0.0,
                              fluo,       1, NULL,    1.0,
                              NULL);
  if( err) {
    lsevents_send_event( "Mode fluorescence Aborted");
    lsredis_sendStatusReport( 1, "Error starting motors");
    return 1;
  }

  err = lspmac_est_move_time_wait( move_time + 10.0, mmask,
                                   cryo,
                                   fluo,
                                   NULL);
  if( err) {
    lsevents_send_event( "Mode fluorescence Aborted");
    lsredis_sendStatusReport( 0, "Gave up waiting for motors");
    return 1;
  }

  lsevents_send_event( "Mode fluorescence Done");
  lsredis_sendStatusReport( 0, "In fluorescence mode");

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
                              apery,     0, "In",    0.0,
                              aperz,     0, "In",    0.0,
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


int md2cmds_phase_fastCentering() {

  lsredis_sendStatusReport( 0, "Starting Fast Centering Mode");
  lsevents_send_event( "Mode fast center Starting");

  //
  // Move 'em
  //
  lspmac_est_move_time( NULL, NULL,
                        apery,     0, "In",    0.0,
                        aperz,     0, "In",    0.0,
                        capy,      0, "In",    0.0,
                        capz,      0, "Out",    0.0,
                        scint,     0, "Cover", 0.0,
                        blight_ud, 1, NULL,    1.0,
                        zoom,      1, NULL,    1.0,
                        cryo,      1, NULL,    0.0,
                        fluo,      1, NULL,    0.0,
                        NULL);
  lsevents_send_event( "Mode fast center done");
  return 0;
}

/* Go to centering mode without change the zoom
 */
int md2cmds_phase_centerNoZoom() {
  double move_time;
  int mmask, err;

  lsredis_sendStatusReport( 0, "Starting Center No Zoom Mode");
  lsevents_send_event( "Mode center no zoom Starting");

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask,
                              apery,     0, "In",    0.0,
                              aperz,     0, "In",    0.0,
                              capy,      0, "In",    0.0,
                              capz,      0, "Out",    0.0,
                              scint,     0, "Cover", 0.0,
                              blight_ud, 1, NULL,    1.0,
                              cryo,      1, NULL,    0.0,
                              fluo,      1, NULL,    0.0,
                              NULL);
  if( err) {
    lsevents_send_event( "Mode center no zoom Aborted");
    return err;
  }

  err = lspmac_est_move_time_wait( move_time + 10.0, mmask,
                                   cryo,
                                   fluo,
                                   NULL);
  if( err) {
    lsevents_send_event( "Mode center no zoom Aborted");
    return err;
  }

  lsevents_send_event( "Mode center no zoom Done");
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
                              aperz,      0, "Out",          0.0,
                              capy,       0, "In",           0.0,
                              capz,       0, "Out",          0.0,
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

int md2cmds_homestages( const char *ccmd) {
  md2cmds_home_prep();
  lspmac_home1_queue(cenx);
  lspmac_home1_queue(ceny);
  lspmac_home1_queue(alignx);
  lspmac_home1_queue(aligny);
  lspmac_home1_queue(alignz);

  if (md2cmds_home_wait( 60.0)) {
    lslogging_log_message( "md2cmds_homestages: Waited for a minutes homing stages.  Got bored.");
    return 1;
  }
  return 0;
}

/** Set centering and alignment motors to a predefined point
 */
int md2cmds_goto_point( const char *ccmd) {
  char *cmd;            // command that brought us here
  char *ignore;         // likely the string 'gotoPoint' since that is how we were called
  char *ptr;            // pointer to the next item to parse in 'cmd'
  char *pts;            // a string version of the point number
  char *endptr;
  int pt;
  int clength;
  double cx, cy, ax, ay, az;
  int err;
  double move_time;
  int mmask;

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
    lslogging_log_message( "md2cmds_goto_point: ignoring blank command '%s'", cmd);
    lsredis_sendStatusReport( 1, "gotoPoint ignoring blank command");
    free( cmd);
    return 1;
  }
  // The first string should be "gotoPoint" cause that's how we got here.
  // Toss it. (It's called 'ignore' for a reason
  
  pts = strtok_r( NULL, " ", &ptr);
  if( pts == NULL) {
    lslogging_log_message( "md2cmds_goto_point: missing point number");
    lsredis_sendStatusReport( 1, "gotoPoint missing point number");
    free( cmd);
    return 1;
  }
  pt = strtol( pts, &endptr, 10);
  if( pts == endptr) {
    lslogging_log_message( "md2cmds_goto_point: cannot parse point number");
    lsredis_sendStatusReport( 1, "gotoPoint cannot parse point number");
    free( cmd);
    return 1;
  }
  
  clength = lsredis_getl( lsredis_get_obj( "centers.length"));
  if( pt < 0 || pt >= clength) {
    lslogging_log_message( "md2cmds_goto_point: invalid point number.  Found %d but centers.length is %d", pt, clength);
    lsredis_sendStatusReport( 1, "gotoPoint: invalid point number.  Found %d but centers.length is %d", pt, clength);
    free( cmd);
    return 1;
  }
  free( cmd);

  cx = lsredis_getd( lsredis_get_obj( "centers.%d.cx", pt));
  cy = lsredis_getd( lsredis_get_obj( "centers.%d.cy", pt));
  ax = lsredis_getd( lsredis_get_obj( "centers.%d.ax", pt));
  ay = lsredis_getd( lsredis_get_obj( "centers.%d.ay", pt));
  az = lsredis_getd( lsredis_get_obj( "centers.%d.az", pt));


  err = lspmac_est_move_time( &move_time, &mmask,
                              cenx,    0, NULL, cx,
                              ceny,    0, NULL, cy,
                              alignx,  0, NULL, ax,
                              aligny,  0, NULL, ay,
                              alignz,  0, NULL, az,
                              NULL);
  if( err) {
    lsevents_send_event( "gotoPoint failed");
    return err;
  }

  err = lspmac_est_move_time_wait( move_time + 5.0, mmask,
                                   NULL);

  if( err) {
    lsevents_send_event( "gotoPoint failed");
    return err;
  }

  lsevents_send_event( "gotoPoint Done");

  //  md2cmds_mvcenter_move( cx, cy, ax, ay, az);

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

  //
  // Just kill all current motions and close the shutter;
  //
  md2cmds_motion_reset();


  if( ccmd == NULL || *ccmd == 0)
    return 1;

  // use a copy as strtok_r modifies the string it is parsing
  // Ignore the first token, that's just the command that called this routing
  //
  cmd = strdup( ccmd);
  ignore = strtok_r( cmd, " ", &ptr);
  if( ignore == NULL) {
    lslogging_log_message( "md2cmds_phase_change: ignoring empty command string (how did we let things get this far?");
    free( cmd);
    return 1;
  }

  //
  // The next token is the mode we wish to put the MD2 in.
  //
  mode = strtok_r( NULL, " ", &ptr);
  if( mode == NULL) {
    lslogging_log_message( "md2cmds_phase_change: no mode specified");
    return 1;
  }


  //
  // Tangled web.  Probably not worth fixing.  O(N) but N is pretty small.
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
  } else if( strcmp( mode, "fastCentering") == 0) {
    err = md2cmds_phase_fastCentering();
  } else if( strcmp( mode, "centerNoZoom") == 0) {
    err = md2cmds_phase_centerNoZoom();
  } else if( strcmp( mode, "fluorescence") == 0) {
    err = md2cmds_phase_fluorescence();
  } else {
    lslogging_log_message( "md2cmds_phase_change: Unknown mode %s", mode);
    return 1;
  }

  lsredis_setstr( lsredis_get_obj( "phase"), err ? "unknown" : mode);

  free( cmd);
  return err;
}

/** Move the centering and alignment tables
 */
void md2cmds_mvcenter_move(
                           double cx,   /**< [in] Requested Centering Table X           */
                           double cy,   /**< [in] Requested Centering Table Y           */
                           double ax,   /**< [in] Requested Alignment Table X           */
                           double ay,   /**< [in] Requested Alignment Table Y           */
                           double az    /**< [in] Requested Alignment Table Z           */
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

  //  ;150              LS-CAT Move X, Y Absolute
  //  ;                 Q20    = X Value
  //  ;                 Q21    = Y Value
  //  ;                 Q100   = 1 << (coord sys no  - 1)

  lspmac_SockSendDPline( "kappaphi_move", "&7 Q20=%d Q21=%d Q100=64", kc, pc);
}


void md2cmds_shutter_open_cb( char *evt) {
  pthread_mutex_lock( &md2cmds_shutter_mutex);
  md2cmds_shutter_open_flag = 1;
  pthread_cond_signal( &md2cmds_shutter_cond);
  pthread_mutex_unlock( &md2cmds_shutter_mutex);
}

void md2cmds_shutter_not_open_cb( char *evt) {
  pthread_mutex_lock( &md2cmds_shutter_mutex);
  md2cmds_shutter_open_flag = 0;
  pthread_cond_signal( &md2cmds_shutter_cond);
  pthread_mutex_unlock( &md2cmds_shutter_mutex);
}


void md2cmds_setAxis(lspmac_motor_t *mp, char axis, int old_coord, int new_coord) {
  int motor_num;

  motor_num = lsredis_getd( mp->motor_num);
  
  lspmac_SockSendDPline( NULL, "&%d #%d->0 &%d #%d->%c", old_coord, motor_num, new_coord, motor_num, axis);

}

/** call up a predifined raster line 
 *
 */

int md2cmds_raster( const char *cmd) {
  static const char *id = "md2cmds_raster";
  int err, i;
  regmatch_t pmatch[16];
  char key[256];
  
  if( strlen(cmd) > sizeof( key)-1) {
    lslogging_log_message( "%s: command too long '%s'", id, cmd);
    return 1;
  }
  
  err = regexec( &md2cmds_cmd_regex, cmd, 16, pmatch, 0);
  if( err) {
    lslogging_log_message( "%s: no match found from '%s'", id, cmd);
    return 1;
  }

  for( i=0; i<16; i++) {
    if( pmatch[i].rm_so == -1)
      continue;
    lslogging_log_message( "%s: %d '%.*s'", id, i, pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
  }

  //
  // get our key
  //
  snprintf( key, sizeof( key)-1, "%.*s", pmatch[4].rm_eo - pmatch[4].rm_so, cmd+pmatch[4].rm_so);
  key[sizeof( key)-1] = 0;

  lsraster_step(key);

  return 0;
}

/** Shutterless data collection
 ** \param dummy Unused
 ** returns non-zero on error
 */
int md2cmds_shutterless( const char *dummy) {
  long long skey;       //!< px.shots key of our exposure
  int sindex;           //!< px.shots sindex of our shot
  double exp_time;      //!< Exposure Time from postgresql in seconds
  //double p6510;               //!< Shutter opening time in msec
  double p6511;         //!< Shutter closing time in msec
  double q1;            //!< Exposure Time in mSec
  double q2;            //!< Acceleration Time in mSecs
  double q3;            //!< Time at constant velocity before triggering detector
  double q4;            //!< Omega velocity in counts/msec
  double q5;            //!< Backup distance
  double q10;           //!< Omega open position in counts
  double q12;           //!< Omega close position in counts
  double q15;           //!< Center X open position in counts
  double q17;           //!< Center X close position in counts
  double q20;           //!< Center Y open position in counts
  double q22;           //!< Center Y close position in counts
  double q25;           //!< Align Y open position in counts
  double q27;           //!< Align Y close position in counts
  double omega_u2c;     //!< Omega degrees to counts conversion
  double omega_np;      //!< Omega home mark position where we measure omega from
  double omega_ma;      //!< our maximum acceleration for omega
  double cx_u2c;        //!< Center X mm to counts conversion
  double cx_np;         //!< Center X neutral position
  double cy_u2c;        //!< Center Y mm to counts conversion
  double cy_np;         //!< Center Y neutral position
  double ay_u2c;        //!< Align Y mm to counts conversion
  double ay_np;         //!< Align Y neutral position
  lsredis_obj_t *collection_running;
  int clength;          //!< Length of centers array
  double cx0, cx1;      //!< Center X points
  double cy0, cy1;      //!< Center Y points
  double ay0, ay1;      //!< Align Y points
  struct timespec now, timeout; //!< setup timeouts for shutter
  int err;
  int ds;
  double move_time;
  int mmask;
  int explore_mode;
  int n_data_files;
  char file_name_temp[256];
  json_t *j_tmp_obj;
  json_t *j_expected_files;
  lsredis_obj_t *expected_files;
  char *expected_files_str;
  int i;

  //
  // Make a guess at the correct value.  TODO: measure and place in
  // redis variables.
  //
  //p6510 =  10;        // Imperical value: use scope to measure fast-shutter/shutter-box edges
  p6511 =   2;

  lslogging_log_message("shutterless 0");

  lsevents_send_event("Shutterless Collection Starting");
  expected_files = lsredis_get_obj("expected_files");

  collection_running = lsredis_get_obj("collection.running");
  lsredis_setstr(collection_running, "True");

  explore_mode = lsredis_getl(lsredis_get_obj("detector.explore_mode"));

  //
  // Go to data collection mode
  //
  lsredis_sendStatusReport( 0, "Putting MD2 in data collection mode");
  if( md2cmds_phase_change( "changeMode dataCollection")) {
    lsredis_sendStatusReport( 1, "Failed to put MD2 into data collection mode within a reasonable amount of time.");
    lsevents_send_event( "Shutterless Collection Aborted");
    lsredis_setstr( collection_running, "False");
    return 1;
  }  

  lslogging_log_message("shutterless 1");
  
  //
  // Set up monitoring of the detector state and, by the way, get the
  // current state.
  //
  pthread_mutex_lock( &detector_state_mutex);
  ds = detector_state_int;
  pthread_mutex_unlock( &detector_state_mutex);

  lslogging_log_message("explore_mode: %d detector_state_int: %d", explore_mode, ds);

  clock_gettime( CLOCK_REALTIME, &now);
  timeout.tv_sec  = now.tv_sec + 20;
  timeout.tv_nsec = now.tv_nsec;
      
  err = 0;
  pthread_mutex_lock(&detector_state_mutex);
  while (err == 0 && ((explore_mode && detector_state_int != 3) || (!explore_mode && detector_state_int != 1))) {
    err = pthread_cond_timedwait(&detector_state_cond, &detector_state_mutex, &timeout);
  }
  pthread_mutex_unlock(&detector_state_mutex);
  
  if (err == ETIMEDOUT) {
    //
    // The detector is not ready or is not armed, abort now
    //
    lsredis_sendStatusReport(1, "Detector does not appear to be ready");
    lslogging_log_message("md2cmds_shutterless: Detector is not in ready state");
    lsevents_send_event("Shutterless Collection Aborted");
    lsredis_setstr(collection_running, "False");
    return 1;
  }
  lslogging_log_message("shutterless 2");

  
  omega_u2c   = lsredis_getd( omega->u2c);
  omega_np    = lsredis_getd( omega->neutral_pos);
  omega_ma    = lsredis_getd( omega->max_accel);
  cx_u2c      = lsredis_getd( cenx->u2c);
  cx_np       = lsredis_getd( cenx->neutral_pos);
  cy_u2c      = lsredis_getd( ceny->u2c);
  cy_np       = lsredis_getd( ceny->neutral_pos);
  ay_u2c      = lsredis_getd( aligny->u2c);
  ay_np       = lsredis_getd( aligny->neutral_pos);
  clength     = lsredis_getl( lsredis_get_obj("centers.length"));
  cx0         = lsredis_getd( lsredis_get_obj("centers.0.cx"));
  cy0         = lsredis_getd( lsredis_get_obj("centers.0.cy"));
  ay0         = lsredis_getd( lsredis_get_obj("centers.0.ay"));
  if (clength <= 1) {
    cx1 = cx0;
    cy1 = cy0;
    ay1 = ay0;
  } else {
    cx1 = lsredis_getd( lsredis_get_obj("centers.1.cx"));
    cy1 = lsredis_getd( lsredis_get_obj("centers.1.cy"));
    ay1 = lsredis_getd( lsredis_get_obj("centers.1.ay"));
  }
  
  while(1) {
    lspg_nextshot_call();
    lspg_nextshot_wait();

    if (lspg_nextshot.query_error) {
      lsredis_sendStatusReport(1, "Cound not retrieve next shot info.");
      lslogging_log_message("md2cmds_shutterless: query error retriveing next shot info. Aborting");
      lsevents_send_event("Shutterless Collection Aborted");
      lsredis_setstr(collection_running, "False");
      return 1;
    }

    lslogging_log_message("shutterless 3: active = %d", lspg_nextshot.active);

    if (lspg_nextshot.no_rows_returned) {
      lslogging_log_message("lspg_nextshot returned no rows");
      lsredis_sendStatusReport(0, "No more images to collect");
      lsredis_setstr(collection_running, "False");
      lspg_nextshot_done();
      break;
    }

    //
    // Calculate the number of datafiles to have the xfer process look for.
    //
    // The number of images per datafile is configurable with the
    // defaule being 1000.  TODO: use redis to store this value, have
    // eigerServer enforce it and read it here instead of assuming
    // it's 1000.
    //
    if (strcmp(lspg_nextshot.stype, "shutterless")==0) {
      n_data_files = (lspg_nextshot.dsfrate * lspg_nextshot.dsrange)/1000 +1;
    } else {
      n_data_files = 1;
    }

    //
    // The loop explorer uses the old interpolation methods which are
    // not compatable with our use of the centering points here.
    // Hopefully just checking the center points array length is
    // enough to catch this mode.  TODO: make it clearer, ie, add
    // another flag to indicate that we are in the explorer mode.
    //
    if( clength <= 1 && lspg_nextshot.active) {
      lsredis_set_preset( "centering.x", "Beam", lspg_nextshot.cx);
      lsredis_set_preset( "centering.y", "Beam", lspg_nextshot.cy);
      lsredis_set_preset( "align.x",     "Beam", lspg_nextshot.ax);
      lsredis_set_preset( "align.y",     "Beam", lspg_nextshot.ay);
      lsredis_set_preset( "align.z",     "Beam", lspg_nextshot.az);

      lsredis_setstr(lsredis_get_obj("centers.0.cx"), "%0.3f", lspg_nextshot.cx);
      lsredis_setstr(lsredis_get_obj("centers.0.cy"), "%0.3f", lspg_nextshot.cy);
      lsredis_setstr(lsredis_get_obj("centers.0.ax"), "%0.3f", lspg_nextshot.ax);
      lsredis_setstr(lsredis_get_obj("centers.0.ay"), "%0.3f", lspg_nextshot.ay);
      lsredis_setstr(lsredis_get_obj("centers.0.az"), "%0.3f", lspg_nextshot.az);


      cx0 = lspg_nextshot.cx;
      cy0 = lspg_nextshot.cy;
      ay0 = lspg_nextshot.ay;

      cx1 = lspg_nextshot.cx;
      cy1 = lspg_nextshot.cy;
      ay1 = lspg_nextshot.ay;

      //
      // Normally ax (focus) and az (spindle height) do not move.
      // However, if, we ever want to then the ax and az code is here.
      // cx, cy, and cz is built into to the PMAC routing and has been
      // stripped out.
      //

      if(
         //
         // Don't move if we are within 0.1 microns of our destination
         //
         (fabs( lspg_nextshot.ax - alignx->position) > 0.0001) ||
         (fabs( lspg_nextshot.az - alignz->position) > 0.0001)) {


        lslogging_log_message( "md2cmds_shutterless: moving center to ax=%f, az=%f", lspg_nextshot.ax, lspg_nextshot.az);

        err = lspmac_est_move_time( &move_time, &mmask,
                                    alignx, 0, NULL, lspg_nextshot.ax,
                                    alignz, 0, NULL, lspg_nextshot.az,
                                    NULL);
        if( err) {
          lsevents_send_event( "Shutterless Collection Aborted");
          lsredis_sendStatusReport( 1, "Failed to start moving to next sample position.");
          lspg_nextshot_done();
          lsredis_setstr( collection_running, "False");
          lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
          return 1;
        }

        err = lspmac_est_move_time_wait( move_time+10, mmask, NULL);
        if( err) {
          lsredis_sendStatusReport( 1, "Moving to next sample position failed.");
          lsevents_send_event( "Shutterless Collection Aborted");
          lspg_nextshot_done();
          lsredis_setstr( collection_running, "False");
          lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
          return 1;
        }
      }
    }

    if ( lspg_nextshot.dssrate <= 0.1) {
      exp_time = lspg_nextshot.dsexp;
    } else {
      if (strcmp(lspg_nextshot.stype, "shutterless")==0) {
        exp_time = lspg_nextshot.dsrange  / lspg_nextshot.dssrate;
      } else {
        exp_time = lspg_nextshot.dsowidth / lspg_nextshot.dssrate;
      }
    }
    skey     = lspg_nextshot.skey;
    sindex   = lspg_nextshot.sindex;
    lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Preparing')", skey);
    lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Preparing\"}", skey);
    lsredis_sendStatusReport( 0, "Preparing Shutterless %d", sindex);
    
    q1  = exp_time * 1000.0;
    q10 = omega_u2c * (lspg_nextshot.sstart + omega_np);
    
    if (strcmp(lspg_nextshot.stype, "shutterless")==0) {
      q12 = omega_u2c * (lspg_nextshot.sstart + lspg_nextshot.dsrange  + omega_np);
    } else {
      q12 = omega_u2c * (lspg_nextshot.sstart + lspg_nextshot.dsowidth + omega_np);
    }
    
    // acceleration time (mSec)
    q2 = ((q12 - q10) / q1) / omega_ma * 2;

    // time to run at constant velocity before triggering the detector (TODO: do we really need this?)
    q3 = 100;

    // alignment y start and end
    q15 = ay_u2c * (ay0 + ay_np);
    q17 = ay_u2c * (ay1 + ay_np);
    
    // centering x start and end
    q20 = cx_u2c * (cx0 + cx_np);
    q22 = cx_u2c * (cx1 + cx_np);
    
    // centering y start and end
    q25 = cy_u2c * (cy0 + cy_np);
    q27 = cy_u2c * (cy1 + cy_np);
    
    if (q1 > 0) {
      q4  = (q12-q10) / q1;     // Omega velocity in counts/msec
      q5  = q4*q2/2;           // Backup distance for Omega (in counts)
    } else {
      q5 = 0;
    }
    
    /*
    ** Move omega to the initial position
    ** TODO: calculate the actual starting position, not just the shutter open position
    */
    if (fabs(lspmac_getPosition( omega)) > 360.0) {
      md2cmds_home_prep();
      lspmac_home1_queue( omega);
      if( md2cmds_home_wait( 20.0)) {
        lslogging_log_message( "md2cmds_shutterless: homing omega timed out.  Shutterless aborted");
        lsevents_send_event( "Shutterless Collection Aborted");
        lsredis_setstr(collection_running, "False");
        lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
        lsredis_setstr( detector_state_redis, "{\"state\": \"Init\", \"expires\": 0}");
        return 1;
      }
    }
    
    err = lspmac_est_move_time( &move_time, &mmask,
                                omega,  0, NULL, lspg_nextshot.sstart - q5/omega_u2c,
                                NULL);
    if( err) {
      lsevents_send_event( "Shutterless Collection Aborted");
      lsredis_sendStatusReport( 1, "Failed to move omega to start position.");
      lspg_nextshot_done();
      lsredis_setstr( collection_running, "False");
      lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
      lsredis_setstr( detector_state_redis, "{\"state\": \"Init\", \"expires\": 0}");
      return 1;
    }
    
    err = lspmac_est_move_time_wait( move_time+10, mmask, NULL);
    if( err) {
      lsredis_sendStatusReport( 1, "Moving omega failed.");
      lsevents_send_event( "Shutterless Collection Aborted");
      lspg_nextshot_done();
      lsredis_setstr( collection_running, "False");
      lsredis_setstr( detector_state_redis, "{\"state\": \"Init\", \"expires\": 0}");
      lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
      return 1;
    }
    
    // We don't need these query results anymore
    lspg_nextshot_done();
    
    lslogging_log_message("shutterless 3...");
    
    //
    // prepare the database and detector to expose On exit theexp
    // detector.state_machine is in the state Arm or, if the detector
    // was not ready, in the state Init.
    //
    if( lspg_eiger_run_prep_all( skey,
                                 kappa->position,
                                 phi->position,
                                 cenx->position,
                                 ceny->position,
                                 alignx->position,
                                 aligny->position,
                                 alignz->position
                                 )) {
      lslogging_log_message( "md2cmds_shutterless: eiger run prep query error, aborting");
      lsredis_sendStatusReport( 1, "Preparing MD2 failed");
      lsevents_send_event( "Shutterless Collection Aborted");
      lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
      lsredis_setstr( detector_state_redis, "{\"state\": \"Init\", \"expires\": 0}");
      lsredis_setstr( collection_running, "False");
      return 1;
    }
    
    lslogging_log_message("shutterless 4");
    
    //
    // Wait for the detector to arm itself
    //
    if (!explore_mode) {
      clock_gettime( CLOCK_REALTIME, &now);
      timeout.tv_sec  = now.tv_sec + 60;
      timeout.tv_nsec = now.tv_nsec;
      
      err = 0;
      pthread_mutex_lock( &detector_state_mutex);
      while (err == 0 && detector_state_int != 3) {
        err = pthread_cond_timedwait( &detector_state_cond, &detector_state_mutex, &timeout);
      }
      pthread_mutex_unlock( &detector_state_mutex);
      
      if( err == ETIMEDOUT) {
        lslogging_log_message( "md2cmds_shutterless: Timed out waiting for detector to be armed.  Shutterless Collection aborted.");
        lsredis_sendStatusReport( 1, "Timed out waiting for detector to be armed.");
        
        lsredis_setstr( detector_state_redis, "{\"state\": \"Init\", \"expires\": 0}");
        
        lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
        lsevents_send_event( "Shutterless Collection Aborted");
        lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
        lsredis_setstr( collection_running, "False");
        return 1;
      }
    }

    lslogging_log_message("shutterless 4b");
    
    md2cmds_setAxis( aligny, 'Y', lsredis_getl( aligny->coord_num), 1);
    md2cmds_setAxis( cenx,   'Z', lsredis_getl( cenx->coord_num),   1);
    md2cmds_setAxis( ceny,   'U', lsredis_getl( ceny->coord_num),   1);
    
    lsredis_sendStatusReport( 0, "Exposing Frames %d", sindex);
    lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Exposing')", skey);
    lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Exposing\"}", skey);
    
    lspmac_set_motion_flags( NULL, omega, NULL);
    lspmac_SockSendDPline( "Exposure",
                           "&1 P6511=%.1f Q1=%.1f Q2=%.1f Q3=%.1f Q10=%.1f Q12=%.1f Q15=%.1f Q17=%.1f Q20=%.1f Q22=%.1f Q25=%.1f Q27=%.1f",
                           p6511, q1, q2, q3, q10, q12, q15, q17, q20, q22, q25, q27
                           );
    
    lspmac_SockSendDPline(  NULL, "B231R");
    
    lslogging_log_message("shutterless 5");
    //
    // wait for the shutter to open
    //
    clock_gettime( CLOCK_REALTIME, &now);
    timeout.tv_sec  = now.tv_sec + (int)(q2/1000+1) + 2;        // round the acceleration time up and add a couple of seconds for the command to be taken 
    timeout.tv_nsec = now.tv_nsec;
    err = 0;
    
    pthread_mutex_lock( &md2cmds_shutter_mutex);
    while( err == 0 && !md2cmds_shutter_open_flag)
      err = pthread_cond_timedwait( &md2cmds_shutter_cond, &md2cmds_shutter_mutex, &timeout);
    
    if( err == ETIMEDOUT) {
      pthread_mutex_unlock( &md2cmds_shutter_mutex);
      lslogging_log_message( "md2cmds_shutterless: Timed out waiting for shutter to open.  Data collection aborted.");
      lspmac_abort();
      lsredis_sendStatusReport( 1, "Timed out waiting for shutter to open.");
      lsredis_setstr( detector_state_redis, "{\"state\": \"Init\", \"expires\": 0}");
      lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
      lsevents_send_event( "Shutterless Collection Aborted");
      lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
      lsredis_setstr( collection_running, "False");
      md2cmds_setAxis( ceny,   lsredis_getc( ceny->axis),   1, lsredis_getl( ceny->coord_num));
      md2cmds_setAxis( cenx,   lsredis_getc( cenx->axis),   1, lsredis_getl( cenx->coord_num));
      md2cmds_setAxis( aligny, lsredis_getc( aligny->axis), 1, lsredis_getl( aligny->coord_num));
      return 1;
    }
    pthread_mutex_unlock( &md2cmds_shutter_mutex);
    
    lslogging_log_message("shutterless 6");
    
    //
    // wait for the shutter to close
    //
    clock_gettime( CLOCK_REALTIME, &now);
    lslogging_log_message( "md2cmds_shutterless: waiting %f seconds for the shutter to close", 4 + exp_time);
    timeout.tv_sec  = now.tv_sec + 10 + ceil(exp_time); // hopefully 10 seconds is long enough to never miss a legitimate shutter close and short enough to bail when something is really wrong
    timeout.tv_nsec = now.tv_nsec;
    
    err = 0;
    
    pthread_mutex_lock( &md2cmds_shutter_mutex);
    while( err == 0 && md2cmds_shutter_open_flag)
      err = pthread_cond_timedwait( &md2cmds_shutter_cond, &md2cmds_shutter_mutex, &timeout);
    
    if( err == ETIMEDOUT) {
      pthread_mutex_unlock( &md2cmds_shutter_mutex);
      lsredis_sendStatusReport( 1, "Timed out waiting for shutter to close.");
      lsredis_setstr( detector_state_redis, "{\"state\": \"Init\", \"expires\": 0}");
      lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
      lslogging_log_message( "md2cmds_shutterless: Timed out waiting for shutter to close.  Shutterless collection aborted.");
      lsevents_send_event( "Shutterless Collection Aborted");
      lsredis_setstr( collection_running, "False");
      lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
      md2cmds_setAxis( ceny,   lsredis_getc( ceny->axis),   1, lsredis_getl( ceny->coord_num));
      md2cmds_setAxis( cenx,   lsredis_getc( cenx->axis),   1, lsredis_getl( cenx->coord_num));
      md2cmds_setAxis( aligny, lsredis_getc( aligny->axis), 1, lsredis_getl( aligny->coord_num));
      return 1;
    }
    pthread_mutex_unlock( &md2cmds_shutter_mutex);
    
    lslogging_log_message("shutterless 7");
    
    if (!explore_mode) {
      //
      // Signal the detector to start reading out
      //
      lsredis_setstr( detector_state_redis, "{\"state\": \"Done\", \"expires\": %lld}", (long long)time(NULL)*1000 + 20000);
      lsredis_sendStatusReport( 0, "Reading Shutterless %d", sindex);
    }
    
    //
    // Update the shot status
    //
    lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Done')", skey);
    lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Done\"}", skey);
    
    lslogging_log_message("shutterless 7.1");
    
    //
    // Return the axes definitions to their original state
    //
    md2cmds_setAxis( ceny,   lsredis_getc( ceny->axis),   1, lsredis_getl( ceny->coord_num));
    md2cmds_setAxis( cenx,   lsredis_getc( cenx->axis),   1, lsredis_getl( cenx->coord_num));
    md2cmds_setAxis( aligny, lsredis_getc( aligny->axis), 1, lsredis_getl( aligny->coord_num));
    
    //
    // reset shutter has opened flag
    //
    lspmac_SockSendDPline( NULL, "P3005=0");

    lslogging_log_message("shutterless 8");
    
    //
    // Wait for the detector to be done
    //
    clock_gettime( CLOCK_REALTIME, &now);
    timeout.tv_sec  = now.tv_sec + 20;
    timeout.tv_nsec = now.tv_nsec;
    
    if (!explore_mode) {
      err = 0;
      pthread_mutex_lock( &detector_state_mutex);
      while (err == 0 && detector_state_int != 4) {
        err = pthread_cond_timedwait( &detector_state_cond, &detector_state_mutex, &timeout);
      }
      pthread_mutex_unlock( &detector_state_mutex);

      if( err == ETIMEDOUT) {
        lslogging_log_message( "md2cmds_shutterless: Timed out waiting for detector to finish up.  Shutterless Collection aborted.");
        lsredis_sendStatusReport( 1, "Timed out waiting for detector to finish.");
      
        lsredis_setstr( detector_state_redis, "{\"state\": \"Init\", \"expires\": 0}");
      
        lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
        lsevents_send_event( "Shutterless Collection Aborted");
        lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
        lsredis_setstr( collection_running, "False");
        return 1;
      }
    }
  }

  j_expected_files = json_array();
  
  snprintf(file_name_temp, sizeof(file_name_temp)-1, "%s/%s_master.h5", lspg_nextshot.dsdir, lspg_nextshot.sfn);
  file_name_temp[sizeof(file_name_temp)-1] = 0;
  j_tmp_obj = json_string(file_name_temp);
  json_array_append_new(j_expected_files, j_tmp_obj);

  for (i=0; i< n_data_files; i++) {
    snprintf(file_name_temp, sizeof(file_name_temp)-1, "%s/%s_data_%06d.h5", lspg_nextshot.dsdir, lspg_nextshot.sfn, i+1);
    file_name_temp[sizeof(file_name_temp)-1] = 0;
    j_tmp_obj = json_string(file_name_temp);
    json_array_append_new(j_expected_files, j_tmp_obj);
  }
  expected_files_str = json_dumps(j_expected_files, 0);
  lsredis_setstr(expected_files, expected_files_str);
  free(expected_files_str);
  json_decref(j_expected_files);

  lslogging_log_message("shutterless Data Collection Expecting %d data files for file %s/%s", n_data_files, lspg_nextshot.dsdir, lspg_nextshot.sfn);

  lslogging_log_message("shutterless Data Collection Done");

  return 0;
}

/** Collect some data
 *  \param dummy Unused
 *  returns non-zero on error
 */
int md2cmds_collect( const char *dummy) {
  long long skey;       //!< shots table key of shot to be taken
  int sindex;           //!< index of shot to be taken
  int issnap;           //!< flag if this is a snap shot instead of a normal shot
  double exp_time;      //!< Exposure time (saved to compute shutter timeout)
  double p170;          //!< start cnts
  double p171;          //!< delta cnts
  double p173;          //!< omega velocity cnts/msec

  double p175;          //!< acceleration time (msec)
  double p180;          //!< exposure time (msec)
  double u2c;           //!< unit to counts conversion
  double neutral_pos;   //!< nominal zero offset
  double max_accel;     //!< maximum acceleration allowed for omega
  double kappa_pos;     //!< current kappa position in case we need to move phi only
  double phi_pos;       //!< current phi position in case we need to move kappa only
  struct timespec now, timeout; //!< setup timeouts for shutter
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
  lspmac_SockSendDPline( NULL, "P3001=0 P3002=0 P3005=0");

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

    skey   = lspg_nextshot.skey;
    sindex = lspg_nextshot.sindex;
    issnap = strcmp( lspg_nextshot.stype, "snap") == 0;
    lslogging_log_message( "md2cmds next shot is %lld  active %d  cx %f  cy %f  ax %f  ay %f  az %f", skey, lspg_nextshot.active, lspg_nextshot.cx,lspg_nextshot.cy,lspg_nextshot.ax,lspg_nextshot.ay,lspg_nextshot.az);
    lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Preparing')", skey);
    lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Preparing\"}", skey);
    lsredis_sendStatusReport( 0, "Preparing %s %d", issnap ? "Snap" : "Frame", sindex);


    if( lspg_nextshot.active) {
      lsredis_set_preset( "centering.x", "Beam", lspg_nextshot.cx);
      lsredis_set_preset( "centering.y", "Beam", lspg_nextshot.cy);
      lsredis_set_preset( "align.x",     "Beam", lspg_nextshot.ax);
      lsredis_set_preset( "align.y",     "Beam", lspg_nextshot.ay);
      lsredis_set_preset( "align.z",     "Beam", lspg_nextshot.az);

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
          lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
          return 1;
        }

        err = lspmac_est_move_time_wait( move_time+10, mmask, NULL);
        if( err) {
          lsredis_sendStatusReport( 1, "Moving to next sample position failed.");
          lsevents_send_event( "Data Collection Aborted");
          //      lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");   // Should we even have the diffractometer lock at this point?
          lspg_nextshot_done();
          lsredis_setstr( collection_running, "False");
          lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
          return 1;
        }
      }
    }

    // Maybe move kappa and/or phi
    //
    if( !lspg_nextshot.dsphi_isnull || !lspg_nextshot.dskappa_isnull) {

      kappa_pos = lspg_nextshot.dskappa_isnull ? lspmac_getPosition( kappa) : lspg_nextshot.dskappa;
      phi_pos   = lspg_nextshot.dsphi_isnull   ? lspmac_getPosition( phi)   : lspg_nextshot.dsphi;

      lsredis_sendStatusReport( 0, "Moving Kappa %s %d", issnap ? "Snap" : "Frame", sindex);
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
        lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
        return 1;
      } 

      err = lspmac_est_move_time_wait( move_time + 10, mmask, NULL);
      if( err) {
        lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
        lsevents_send_event( "Data Collection Aborted");
        lsredis_sendStatusReport( 1, "Moving Kappa timed out");
        lspg_nextshot_done();
        lsredis_setstr( collection_running, "False");
        lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
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
      lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
      lsredis_setstr( collection_running, "False");
      return 1;
    }
    
    //
    // make sure our opened flag is down
    // wait for the p3005=0 command to be noticed
    //
    clock_gettime( CLOCK_REALTIME, &now);
    timeout.tv_sec  = now.tv_sec + 10;
    timeout.tv_nsec = now.tv_nsec;

    err = 0;

    pthread_mutex_lock( &md2cmds_shutter_mutex);
    if( md2cmds_shutter_open_flag) {
      fshut->moveAbs( fshut, 0);
      while( err == 0 && md2cmds_shutter_open_flag)
        err = pthread_cond_timedwait( &md2cmds_shutter_cond, &md2cmds_shutter_mutex, &timeout);

      if( err == ETIMEDOUT) {
        pthread_mutex_unlock( &md2cmds_shutter_mutex);
        lsredis_sendStatusReport( 1, "Timed out waiting for shutter closed confirmation.");
        lslogging_log_message( "md2cmds_collect: Timed out waiting for shutter to be confirmed closed.  Data collection aborted.");
        lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
        lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");
        lsevents_send_event( "Data Collection Aborted");
        lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
        lsredis_setstr( collection_running, "False");
        return 1;
      }
    }
    pthread_mutex_unlock( &md2cmds_shutter_mutex);

    //
    // Wait for the detector to drop its lock indicating that it is ready for the exposure
    //
    lspg_lock_detector_all();
    lspg_unlock_detector_all();


    //
    // Start the exposure
    //
    lsredis_sendStatusReport( 0, "Exposing %s %d", issnap ? "Snap" : "Frame", sindex);
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

    pthread_mutex_lock( &md2cmds_shutter_mutex);
    while( err == 0 && !md2cmds_shutter_open_flag)
      err = pthread_cond_timedwait( &md2cmds_shutter_cond, &md2cmds_shutter_mutex, &timeout);

    if( err == ETIMEDOUT) {
      pthread_mutex_unlock( &md2cmds_shutter_mutex);
      lslogging_log_message( "md2cmds_collect: Timed out waiting for shutter to open.  Data collection aborted.");
      lsredis_sendStatusReport( 1, "Timed out waiting for shutter to open.");
      lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");
      lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
      lsevents_send_event( "Data Collection Aborted");
      lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
      lsredis_setstr( collection_running, "False");
      return 1;
    }
    pthread_mutex_unlock( &md2cmds_shutter_mutex);


    //
    // wait for the shutter to close
    //
    clock_gettime( CLOCK_REALTIME, &now);
    lslogging_log_message( "md2cmds_collect: waiting %f seconds for the shutter to close", 4 + exp_time);
    timeout.tv_sec  = now.tv_sec + 4 + ceil(exp_time);  // hopefully 4 seconds is long enough to never miss a legitimate shutter close and short enough to bail when something is really wrong
    timeout.tv_nsec = now.tv_nsec;

    err = 0;

    pthread_mutex_lock( &md2cmds_shutter_mutex);
    while( err == 0 && md2cmds_shutter_open_flag)
      err = pthread_cond_timedwait( &md2cmds_shutter_cond, &md2cmds_shutter_mutex, &timeout);

    if( err == ETIMEDOUT) {
      pthread_mutex_unlock( &md2cmds_shutter_mutex);
      lsredis_sendStatusReport( 1, "Timed out waiting for shutter to close.");
      lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");
      lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Error')", skey);
      lslogging_log_message( "md2cmds_collect: Timed out waiting for shutter to close.  Data collection aborted.");
      lsevents_send_event( "Data Collection Aborted");
      lsredis_setstr( collection_running, "False");
      lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Error\"}", skey);
      return 1;
    }
    pthread_mutex_unlock( &md2cmds_shutter_mutex);

    //
    // Signal the detector to start reading out
    //
    lspg_query_push( NULL, NULL, "SELECT px.unlock_diffractometer()");
    lsredis_sendStatusReport( 0, "Reading %s %d", issnap ? "Snap" : "Frame", sindex);

    //
    // Update the shot status
    //
    lspg_query_push( NULL, NULL, "SELECT px.shots_set_state(%lld, 'Writing')", skey);
    lsredis_setstr( lsredis_get_obj( "detector.state"), "{\"skey\": %lld, \"sstate\": \"Writing\"}", skey);

    //
    // reset shutter has opened flag
    //
    lspmac_SockSendDPline( NULL, "P3005=0");

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
  lsredis_sendStatusReport( 0, "Finished data collection.");
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

      ax  += lspg_getcenter.dax;
      lsredis_set_preset( "align.x", "Beam", ax);
      lsredis_set_preset( "align.x", "Back", ax + bax);
    }      


    if( lspg_getcenter.day_isnull == 0) {
      err = lsredis_find_preset( "align.y", "Back_Vector", &bay);
      if( err == 0)
        bay = 0.0;

      ay  += lspg_getcenter.day;
      lsredis_set_preset( "align.y", "Beam", ay);
      lsredis_set_preset( "align.y", "Back", ay + bay);
    }
                          
    if( lspg_getcenter.daz_isnull == 0) {
      err = lsredis_find_preset( "align.z", "Back_Vector", &baz);
      if( err == 0)
        baz = 0.0;

      az  += lspg_getcenter.daz;
      lsredis_set_preset( "align.z", "Beam", az);
      lsredis_set_preset( "align.z", "Back", az + baz);
    }
  }
  lspg_getcenter_done();

  if( lspmac_est_move_time( &move_time, &mmask,
                            scint,  1,  "Cover", 0.0,
                            capz,   1,  "Out",   0.0,
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

  move_time += 3;
  if( lspmac_est_move_time_wait( move_time, mmask,
                                 scint,
                                 capz,
                                 zoom,
                                 NULL)) {
    lslogging_log_message( "md2cmds_rotate: organ motion timed out %f seconds", move_time);
    lsevents_send_event( "Rotate Aborted");
    return 1;
  }

  if( md2cmds_home_wait( 20.0)) {
    lslogging_log_message( "md2cmds_rotate: homing motors timed out.  Rotate aborted");
    lsevents_send_event( "Rotate Aborted");
    return 1;
  }

  lsredis_setstr( lsredis_get_obj( "phase"), "center");

  // Report new center positions
  cx = lspmac_getPosition( cenx);
  cy = lspmac_getPosition( ceny);
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);
  lspg_query_push( NULL, NULL, "SELECT px.applycenter( %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f)", cx, cy, ax, ay, az, lspmac_getPosition(kappa), lspmac_getPosition( phi));

  md2cmds_setsamplebeam( "dummy argument");

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

      ax  += lspg_getcenter.dax;
      lsredis_set_preset( "align.x", "Beam", ax);
      lsredis_set_preset( "align.x", "Back", ax + bax);

    }      


    if( lspg_getcenter.day_isnull == 0) {
      err = lsredis_find_preset( "align.y", "Back_Vector", &bay);
      if( err == 0)
        bay = 0.0;

      ay  += lspg_getcenter.day;
      lsredis_set_preset( "align.y", "Beam", ay);
      lsredis_set_preset( "align.y", "Back", ay + bay);
    }
                          
    if( lspg_getcenter.daz_isnull == 0) {
      err = lsredis_find_preset( "align.z", "Back_Vector", &baz);
      if( err == 0)
        baz = 0.0;

      az  += lspg_getcenter.daz;
      lsredis_set_preset( "align.z", "Beam", az);
      lsredis_set_preset( "align.z", "Back", az + baz);
    }
  }
  lspg_getcenter_done();

  if( lspmac_est_move_time( &move_time, &mmask,
                            scint,  1,  "Cover", 0.0,
                            capz,   1,  "Out", 0.0,
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

  move_time += 3;
  if( lspmac_est_move_time_wait( move_time, mmask,
                                 scint,
                                 capz,
                                 zoom,
                                 NULL)) {
    lslogging_log_message( "md2cmds_nonrotate: organ motion timed out %f seconds", move_time);
    lsevents_send_event( "Local Centering Aborted");
    return 1;
  }

  lsredis_setstr( lsredis_get_obj( "phase"), "center");

  // Report new center positions
  cx = lspmac_getPosition( cenx);
  cy = lspmac_getPosition( ceny);
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);
  lspg_query_push( NULL, NULL, "SELECT px.applycenter( %.3f, %.3f, %.3f, %.3f, %.3f, %.3f, %.3f)", cx, cy, ax, ay, az, lspmac_getPosition(kappa), lspmac_getPosition( phi));


  md2cmds_moveRel( "moveRel omega -180");

  md2cmds_setsamplebeam( "dummy argument");
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
  static struct timespec capz_timestarted;      //!< track the time spent moving capz
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
  double ax, ay, az;    // current positions
  double axb, ayb, azb; // preset Beam positions
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

/** set the current position of the alignment and centering stages as the "Beam" preset
 *
*/
int md2cmds_setsamplebeam( const char *cmd) {
  double ax, ay, az, cx, cy;    // current positions
  
  //
  // get the current position
  //
  ax = lspmac_getPosition( alignx);
  ay = lspmac_getPosition( aligny);
  az = lspmac_getPosition( alignz);
  cx = lspmac_getPosition( cenx);
  cy = lspmac_getPosition( ceny);

  lsredis_set_preset( "align.x",     "Beam", ax);
  lsredis_set_preset( "align.y",     "Beam", ay);
  lsredis_set_preset( "align.z",     "Beam", az);
  lsredis_set_preset( "centering.x", "Beam", cx);
  lsredis_set_preset( "centering.y", "Beam", cy);

  return 0;
}

/** Set the beamstop limits that plcc 0 checks for the shutter enable
 *  signal
 */

int md2cmds_setbeamstoplimits( const char *cmd) {
  double u2c;
  double position;
  int upper_limit;
  int lower_limit;

  u2c      = lsredis_getd( capz->u2c);
  position = lspmac_getPosition( capz);
  
  upper_limit = (position + 0.1) * u2c;
  lower_limit = (position - 0.1) * u2c;

  lsredis_setstr( lsredis_get_obj("fastShutter.capzLowCts"),  "%d", lower_limit);
  lsredis_setstr( lsredis_get_obj("fastShutter.capzHighCts"), "%d", upper_limit);

  lspmac_SockSendDPline( NULL, "P6000=%d P6001=%d", lower_limit, upper_limit);

  return 0;
}

int md2cmds_set( const char *cmd) {
  static const char *id = "md2cmds_set";
  int err, i;
  lspmac_motor_t *mp;
  regmatch_t pmatch[32];
  char motor_name[64];
  char preset_name[64];
  char cp[512];
  double rp;
  int preset_index;
  
  if( strlen(cmd) > sizeof( cp)-1) {
    lslogging_log_message( "%s: command too long '%s'", id, cmd);
    return 1;
  }

  lslogging_log_message( "%s: recieved '%s'", id, cmd);

  err = regexec( &md2cmds_cmd_set_regex, cmd, 32, pmatch, REG_EXTENDED);
  if( err) {
    lslogging_log_message( "%s: no match found from '%s'", id, cmd);
    return 1;
  }

  //
  // Find the last entry as that is the name of our preset
  //
  preset_index = -1;
  for( i=1; i<32; i++) {
    if( (pmatch[i].rm_so == -1) || (pmatch[i].rm_eo == pmatch[i].rm_so)) {
      preset_index = i-1;
      break;
    }
    lslogging_log_message( "%s: %d '%.*s'", id, i, pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
  }

  if (preset_index < 3) {
    lslogging_log_message("%s: bad preset index %d", id, preset_index);
    return 1;
  }

  //
  // get preset name
  //
  snprintf(preset_name, sizeof(preset_name)-1, "%.*s", pmatch[preset_index].rm_eo - pmatch[preset_index].rm_so, cmd+pmatch[preset_index].rm_so);
  preset_name[sizeof(preset_name)-1] = 0;

  lslogging_log_message("%s: preset name: %s", id, preset_name);

  //
  // Loop through the motors and set the preset to the current position
  //
  for(i=2; i<preset_index; i++) {
    snprintf(motor_name, sizeof(motor_name)-1, "%.*s", pmatch[i].rm_eo - pmatch[i].rm_so, cmd+pmatch[i].rm_so);
    motor_name[sizeof(motor_name)-1] = 0;
    lslogging_log_message("%s: motor name: %s", id, motor_name);

    mp = lspmac_find_motor_by_name( motor_name);
    if( mp == NULL) {
      lslogging_log_message( "%s: could not find motor '%s'", id, cp);
      return 1;
    }

    rp = lsredis_getd( mp->redis_position);
    lsredis_set_preset( motor_name, preset_name, rp);
  }
  return 0;
}


/** Our worker thread
 */
void *md2cmds_worker(
                     void *dummy                /**> [in] Unused but required by protocol               */
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

  pthread_mutex_init( &md2cmds_shutter_mutex, &mutex_initializer);
  pthread_cond_init(  &md2cmds_shutter_cond, NULL);


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
  err = regcomp( &md2cmds_cmd_set_regex, " *([^ ]+) +([^ ]*) +([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*) *([^ ]*)", REG_EXTENDED);
  if( err != 0) {
    int nerrmsg;
    char *errmsg;

    nerrmsg = regerror( err, &md2cmds_cmd_set_regex, NULL, 0);
    if( nerrmsg > 0) {
      errmsg = calloc( nerrmsg, sizeof( char));
      nerrmsg = regerror( err, &md2cmds_cmd_set_regex, errmsg, nerrmsg);
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

  lsevents_add_listener( "^Reset queued$", md2cmds_motion_reset_cb);

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
  lsevents_add_listener( "^cam.zoom Moving$",           md2cmds_set_scale_cb);
  lsevents_add_listener( "^LSPMAC Done Initializing$",  md2cmds_lspmac_ready_cb);
  lsevents_add_listener( "^Abort Requested$",           md2cmds_lspmac_abort_cb);
  lsevents_add_listener( "^Quitting Program$",          md2cmds_quitting_cb);
  lsevents_add_listener( "^Shutter Open$",              md2cmds_shutter_open_cb);
  lsevents_add_listener( "^Shutter Not Open$",          md2cmds_shutter_not_open_cb);
  lsredis_sendStatusReport( 0, "MD2 Started");


  return &md2cmds_thread;
}
