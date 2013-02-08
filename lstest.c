#include "pgpmac.h"

/*! \file lspmac.c
 *  \brief Test suite for the pgpmac routines
 *  \date 2013
 *  \author Keith Brister
 *  \copyright All Rights Reserved

 A place to put unit tests.
*/



void lstest_lspmac_est_move_time() {
  int err;
  double move_time;
  double fudge;
  int mmask;

  fudge = 2.0;

  mmask = 0;
  err = lspmac_est_move_time( &move_time, &mmask, omega, 0, NULL, 360., NULL);
  lslogging_log_message( "lstest_lspmac_est_move_time: omega 360  move_time=%f  err=%d", move_time, err);
  
  if( lspmac_est_move_time_wait( move_time + fudge, mmask, NULL)) {
    lslogging_log_message( "lstest_lspmac_est_move_time: timed out");
    return;
  }

  err = lspmac_est_move_time( &move_time, &mmask, aperz, 0, "Cover", 0., NULL);
  lslogging_log_message( "lstest_lspmac_est_move_time: aperz Cover move_time=%f err=%d", move_time, err);

  if( lspmac_est_move_time_wait( move_time + fudge, mmask, NULL)) {
    lslogging_log_message( "lstest_lspmac_est_move_time: timed out");
    return;
  }

  err = lspmac_est_move_time( &move_time, &mmask, aperz, 0, "In", 0., NULL);
  lslogging_log_message( "lstest_lspmac_est_move_time: aperz In    move_time=%f err=%d", move_time, err);

  if( lspmac_est_move_time_wait( move_time + fudge, mmask, NULL)) {
    lslogging_log_message( "lstest_lspmac_est_move_time: timed out");
    return;
  }

  err = lspmac_est_move_time( &move_time, &mmask, capz, 0, "Cover", 0., NULL);
  lslogging_log_message( "lstest_lspmac_est_move_time: capz Cover  move_time=%f err=%d", move_time, err);

  if( lspmac_est_move_time_wait( move_time + fudge, mmask, NULL)) {
    lslogging_log_message( "lstest_lspmac_est_move_time: timed out");
    return;
  }


  err = lspmac_est_move_time( &move_time, &mmask, capz, 0, "In", 0., NULL);
  lslogging_log_message( "lstest_lspmac_est_move_time: capz In     move_time=%f err=%d", move_time, err);

  if( lspmac_est_move_time_wait( move_time + fudge, mmask, NULL)) {
    lslogging_log_message( "lstest_lspmac_est_move_time: timed out");
    return;
  }

  err = lspmac_est_move_time( &move_time, &mmask, apery, 0, "In", 0.0, aperz, 0, "In", 0.0, capy, 0, "In", 0.0, capz, 0, "In", 0.0, scint, 0, "Scintillator", 0.0, NULL);
  lslogging_log_message( "lstest_lspmac_est_move_time: apery In aperz In capy In capz In scint Scintillator move_time=%f err=%d", move_time, err);

  if( lspmac_est_move_time_wait( move_time + fudge, mmask, NULL)) {
    lslogging_log_message( "lstest_lspmac_est_move_time: timed out");
    return;
  }
  err = lspmac_est_move_time( &move_time, &mmask, apery, 0, "In", 0.0, aperz, 0, "Cover", 0.0, capy, 0, "In", 0.0, capz, 0, "Cover", 0.0, scint, 0, "Cover", 0.0, NULL);
  lslogging_log_message( "lstest_lspmac_est_move_time: apery Cover aperz Cover capy Cover capz Cover scint Cover move_time=%f err=%d", move_time, err);

  if( lspmac_est_move_time_wait( move_time + fudge, mmask, NULL)) {
    lslogging_log_message( "lstest_lspmac_est_move_time: timed out");
    return;
  }

  err = lspmac_est_move_time( &move_time, &mmask, apery, 1, "In", 0.0, aperz, 1, "In", 0.0, capy, 1, "In", 0.0, capz, 1, "In", 0.0, scint, 1, "Scintillator", 0.0,
			      omega, 0, "manualMount", 0.0, kappa, 0, "manualMount", 0.0, NULL);
  lslogging_log_message( "lstest_lspmac_est_move_time: apery In aperz In capy In capz In scint Scintillator omega manualMount kappa Manualmount move_time=%f err=%d", move_time, err);

  if( lspmac_est_move_time_wait( move_time + fudge, mmask, NULL)) {
    lslogging_log_message( "lstest_lspmac_est_move_time: timed out");
    return;
  }

}



void lstest_main() {
  lstest_lspmac_est_move_time();
}
