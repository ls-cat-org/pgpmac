#include "pgpmac.h"


pthread_cond_t  md2cmds_cond;	// condition to signal when it's time to run an md2 command
pthread_mutex_t md2cmds_mutex;	// mutex for the condition

pthread_cond_t  md2cmds_pg_cond;	// coordinate call and response
pthread_mutex_t md2cmds_pg_mutex;	// message passing between md2cmds and pg

char md2cmds_cmd[MD2CMDS_CMD_LENGTH];	// our command;

static pthread_t md2cmds_thread;



void md2cmds_transfer() {
}

void md2cmds_collect() {
  long long skey;

  lspg_nextshot_call();

  //
  // This is where we'd tell the md2 to move the organs into position
  //

  lspg_nextshot_wait();

  skey = lspg_nextshot.skey;
  lspg_query_push( NULL, "SELECT px.shots_set_state(%lld, 'Preparing')", skey);

  if( lspg_nextshot.active) {
    //
    // Move the alignment and centering tables here
    //
  }

  if( !lspg_nextshot.dsphi_isnull) {
    // move phi here
  }
  
  if( !lspg_nextshot.dskappa_isnull) {
    // move kappa here
  }

  //
  // wait for the detector to grab its lock indicating that it is clearing
  // ready for us to do something
  //
  lspg_wait_for_detector_all();

  //
  // grab the diffractometer lock so the detector can block waiting for us to release
  // when the exposure is done
  //
  lspg_lock_diffractometer_all();

  lspg_query_push( NULL, "SELECT px.shots_set_expose( %lld)", skey);

  //
  // TODO: Grab kappa and phi values and put them here instead of zeros
  //
  lspg_query_push( NULL, "SELECT px.shots_set_params( %lld, 0.0, 0.0)", skey);

  //
  // tell the detector to get to work
  //
  lspg_query_push( NULL, "SELECT px.pushQueue( 'collect %lld')", skey);

  //
  // The detector should own its own lock until it's ready for us to start the exposure
  //
  lspg_lock_detector_all();

  //
  // we don't really need the lock after all
  //
  lspg_query_push( NULL, "SELECT px.unlock_detector()");

  //
  // TODO: Should check to see the if the detector is still on
  //
  
  //
  // TODO: add code to start the exposure here
  //

  //
  // TODO: wait for shutter to close here
  //

  
  lspg_query_push( NULL, "SELECT px.unlock_diffractometer()");

  lspg_query_push( NULL, "SELECT px.shots_set_state(%lld, 'Writing')", skey);

  //
  // TODO:
  // wait for omega to stop moving then position it for the next frame
  //

}

void md2cmds_rotate() {
}

void md2cmds_center() {
}



void *md2cmds_worker( void *dummy) {

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
    }

    md2cmds_cmd[0] = 0;
  }
}





void md2cmds_init() {
  memset( md2cmds_cmd, 0, sizeof( md2cmds_cmd));

  pthread_mutex_init( &md2cmds_mutex, NULL);
  pthread_cond_init( &md2cmds_cond, NULL);

  pthread_mutex_init( &md2cmds_pg_mutex, NULL);
  pthread_cond_init( &md2cmds_pg_cond, NULL);

}

void md2cmds_run() {
  pthread_create( &md2cmds_thread, NULL, md2cmds_worker, NULL);
}
