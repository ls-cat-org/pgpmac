/*! \file pgpmac.h
 *  \brief Headers for the entire pgpmac project
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <netinet/in.h>
#include <errno.h>
#include <poll.h>
#include <libpq-fe.h>
#include <ncurses.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <errno.h>
#include <sys/time.h>
#include <time.h>
#include <getopt.h>
#include <regex.h>
#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <search.h>
#include <ctype.h>

/** Redis Object
 *  Basic object whose value is sychronized with our redis db
 */
typedef struct lsredis_obj_struct {
  pthread_mutex_t mutex;				//!< Don't let anyone use an old value
  pthread_cond_t cond;					//!< wait for a valid value
  struct lsredis_obj_struct *next;			//!< the next in our list (I guess this is going to be a linked list)
  char valid;						//!< 1 if we think the value is good, 0 otherwise
  int wait_for_me;					//!< Number of times we need to see our publication before we start accepting new values
  char *key;						//!< The redis key for this object
  char *events_name;					//!< Name used to generate events (normally key without the station id)
  int value_length;					//!< Number of bytes allocated for value (not value's string length)
  char *value;						//!< our value
  double dvalue;					//!< our value as a double
  long int lvalue;					//!< our value as a long
  char **avalue;					//!< our value as an array of strings
  int bvalue;						//!< our value as a boolean (1 or 0) -1 means we couldn't figure it out
  char cvalue;						//!< just the first character of our value
  int hits;						//!< number of times we've searched for this key
} lsredis_obj_t;

//! Number of status box rows
#define LS_DISPLAY_WINDOW_HEIGHT 8


//! Number of status box columns
#define LS_DISPLAY_WINDOW_WIDTH  24

//! Fixed length postgresql query strings.  Queries should all be function calls so this is not as weird as one might think.
#define LS_PG_QUERY_STRING_LENGTH 1024

//! Fixed length for event names: simplifies string handling
#define LSEVENTS_EVENT_LENGTH   256

/** PMAC ethernet packet definition.
 *
 * Taken directly from the Delta Tau documentation.
 */
typedef struct tagEthernetCmd {
  unsigned char  RequestType;	//!< VR_UPLOAD or VR_DOWNLOAD.
  unsigned char  Request;	//!< The command to run (VR_PMAC_GETMEM, etc).
  unsigned short wValue;	//!< Command parameter 1.
  unsigned short wIndex;	//!< Command parameter 2.
  unsigned short wLength;	//!< Number of bytes in bData.
  unsigned char  bData[1492];	//!< The data buffer, if required.
} pmac_cmd_t;

/** PMAC command queue item.
 *
 * Command queue items are fixed length to simplify memory management.
 */
typedef struct lspmac_cmd_queue_struct {
  pmac_cmd_t pcmd;				//!< the pmac command to send
  int no_reply;					//!< 1 = no reply is expected, 0 = expect a reply
  struct timespec time_sent;			//!< time this item was dequeued and sent to the pmac
  char *event;					//!< event name to send
  void (*onResponse)(struct lspmac_cmd_queue_struct *,int, char *);	//!< function to call when response is received.  args are (int fd, nreturned, buffer)
} pmac_cmd_queue_t;


#define LSPMAC_MAGIC_NUMBER 0x9700436
/** Motor information.
 *
 * A catchall for motors and motor like objects.
 * Not all members are used by all objects.
 */
typedef struct lspmac_motor_struct {
  int magic;					//!< magic number identifying this as a motor structure
  pthread_mutex_t mutex;			//!< coordinate waiting for motor to be done
  pthread_cond_t cond;				//!< used to signal when a motor is done moving
  int not_done;					//!< set to 1 when request is queued, zero after motion has toggled
  void (*read)( struct lspmac_motor_struct *);	//!< method to read the motor status and position
  int command_sent;				//!< Motion command verified sent to pmac
  int motion_seen;				//!< set to 1 when motion has been verified to have started
  pmac_cmd_queue_t *pq;				//!< the queue item requesting motion.  Used to check time request was made
  int homing;					//!< Homing routine started
  int requested_pos_cnts;			//!< requested position
  int *actual_pos_cnts_p;			//!< pointer to the md2_status structure to the actual position
  int actual_pos_cnts;				//!< local copy of actual counts so only our mutex is needed to read
  double position;				//!< scaled position
  double reported_pg_position;			//!< previous position reported to postgresql
  double reported_position;			//!< previous position reported to redis
  double requested_position;			//!< The position as requested by the user
  int *status1_p;				//!< First 24 bit PMAC motor status word
  int status1;					//!< local copy of status1
  int *status2_p;				//!< Sectond 24 bit PMAC motor status word
  int status2;					//!< local copy of status2
  char *dac_mvar;				//!< controlling mvariable as a string
  char *name;					//!< Name of motor as refered by ls database kvs table
  lsredis_obj_t *active;			//!< Use the motor ("true") or not ("false")
  lsredis_obj_t *active_init;			//!< pmac commands to make this motor active
  lsredis_obj_t *axis;				//!< the axis (X, Y, Z, etc) or null if not in a coordinate system
  lsredis_obj_t *coord_num;			//!< coordinate system this motor belongs to (0 if none)
  lsredis_obj_t *home;				//!< pmac commands to home motor
  lsredis_obj_t *inactive_init;			//!< pmac commands to inactivate the motor
  lsredis_obj_t *in_position_band;		//!< moves within this amount are ignored UNITS ARE 1/16 COUNT
  lsredis_obj_t *max_accel;			//!< our maximum acceleration (cts/msec^2)
  lsredis_obj_t *max_pos;			//!< our maximum position (soft limit)
  lsredis_obj_t *max_speed;			//!< our maximum speed (cts/msec)
  lsredis_obj_t *min_pos;			//!< our minimum position (soft limit)
  lsredis_obj_t *motor_num;			//!< pmac motor number
  lsredis_obj_t *neutral_pos;			//!< zero offset
  lsredis_obj_t *pos_limit_hit;			//!< positive limit status
  lsredis_obj_t *neg_limit_hit;			//!< negative limit status
  lsredis_obj_t *precision;			//!< moves of less than this amount may be ignored
  lsredis_obj_t *printf_fmt;			//!< printf format
  lsredis_obj_t *redis_fmt;			//!< special format string to create text array for putting the position back into redis
  lsredis_obj_t *redis_position;		//!< how we report our position to the world
  lsredis_obj_t *status_str;			//!< A talky version of the status
  lsredis_obj_t *u2c;				//!< conversion from counts to units: 0.0 means not loaded yet
  lsredis_obj_t *unit;				//!< string to use as the units
  lsredis_obj_t *update_resolution;		//!< Change needs to be at least this big to report as a new position to the database
  char *write_fmt;				//!< Format string to write requested position to PMAC used for binary io
  int *read_ptr;				//!< With read_mask finds bit to read for binary i/o
  int read_mask;				//!< With read_ptr find bit to read for binary i/o
  int (*moveAbs)( struct lspmac_motor_struct *, double);	//!< function to move the motor
  int (*jogAbs)( struct lspmac_motor_struct *, double);		//!< function to move the motor
  double *lut;					//!< lookup table (instead of u2c)
  int  nlut;					//!< length of lut
  WINDOW *win;					//!< our ncurses window
} lspmac_motor_t;


/** Storage for binary inputs.
 */
typedef struct lspmac_bi_struct {
  int *ptr;			//!< points to the location in the status buffer
  pthread_mutex_t mutex;	//!< so we don't get confused
  int mask;			//!< mask for the bit in the status register
  int position;			//!< the current value.
  int previous;			//!< the previous value
  int first_time;		//!< flag indicating we've not read the input even once
  char *changeEventOn;		//!< Event to send when the value changes to 1
  char *changeEventOff;		//!< Event to send when the value changes to 0
} lspmac_bi_t;


/** Store each query along with it's callback function.
 *  All calls are asynchronous
 */
typedef struct lspgQueryQueueStruct {
  char qs[LS_PG_QUERY_STRING_LENGTH];						//!< our queries should all be pretty short as we'll just be calling functions: fixed length here simplifies memory management
  void (*onResponse)( struct lspgQueryQueueStruct *qq, PGresult *pgr);		//!< Callback function for when a query returns a result
} lspg_query_queue_t;

typedef struct lspg_waitcryo_struct {
  pthread_mutex_t mutex;	//!< practice safe threading
  pthread_cond_t cond;		//!< for signaling
  int new_value_ready;		//!< OK, there is never a value, we need a variable for the conditional wait and this is what we call it everywhere else
} lspg_waitcryo_t;

extern lspg_waitcryo_t lspg_waitcryo;

typedef struct lspg_getcurrentsampleid_struct {
  pthread_mutex_t mutex;		//!< practice safe threading
  pthread_cond_t cond;			//!< for signaling
  int no_rows_returned;			//!< flag for an empty return
  int new_value_ready;			//!< OK, there is never a value, we need a variable for the conditional wait and this is what we call it everywhere else
  unsigned int getcurrentsampleid;	//!< the sample we think is mounted on the diffractometer
  int getcurrentsampleid_isnull  ;	//!< the sample we think is mounted on the diffractometer
} lspg_getcurrentsampleid_t;

extern lspg_getcurrentsampleid_t lspg_getcurrentsampleid;


typedef struct lspg_demandairrights_struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  int new_value_ready;
} lspg_demandairrights_t;

extern lspg_demandairrights_t lspg_demandairrights;


/** Storage for getcenter query
 *  Used for the md2 ROTATE command
 *  that generates the centering movies
 */

typedef struct lspg_getcenter_struct {
  pthread_mutex_t mutex;	//!< don't let the threads collide!
  pthread_cond_t  cond;		//!< provides signaling for when the query is done
  int new_value_ready;		//!< used with condition
  int no_rows_returned;		//!< flag in case no centering information was forthcoming

  int zoom;			//!< the next zoom level to go to before taking the next movie
  int zoom_isnull;

  double dcx;			//!< center x change
  int dcx_isnull;

  double dcy;			//!< center y change
  int dcy_isnull;

  double dax;			//!< alignment x change
  int dax_isnull;

  double day;			//!< alignment y change
  int day_isnull;

  double daz;			//!< alignment z change
  int daz_isnull;
  
} lspg_getcenter_t;

extern lspg_getcenter_t lspg_getcenter;


/**
 * returns 1 if transfer can continue
 *         0 to abort
 */
typedef struct lspg_starttransfer_struct {
  pthread_mutex_t mutex;	//!< Our mutex
  pthread_cond_t cond;		//!< Our condition
  int new_value_ready;		//!< flag for our condition
  int no_rows_returned;		//!< just in case, though this query should always return an integer, perhaps 0

  unsigned int starttransfer;	//!< sample number (4 8-bit segments: station, dewar (lid), puck, and position in the puck)
} lspg_starttransfer_t;

extern lspg_starttransfer_t lspg_starttransfer;

/** Returns the next sample number
 *  Just a 32 bit int (Ha!, take that, nextshot!)
 */
typedef struct lspg_nextsample_struct {
  pthread_mutex_t mutex;	//!< Our mutex
  pthread_cond_t cond;		//!< Our condition
  int new_value_ready;		//!< flag for our condition
  int no_rows_returned;		//!< just in case, though this query should always return an integer, perhaps 0

  unsigned int nextsample;	//!< sample number (4 8-bit segments: station, dewar (lid), puck, and position in the puck)
  int nextsample_isnull;	//!< shouldn't ever be set, but if we change the logic of this call in PG then we are ready for it here.
} lspg_nextsample_t;

extern lspg_nextsample_t lspg_nextsample;

/** Storage definition for nextshot query.
 *
 * The next shot query returns all the information needed to collect the next data frame.
 * Since SQL allows for null fields independently from blank strings a separate
 * integer is used as a flag for this case.  This adds to the program complexity but
 * allows for some important cases.  Suck it up.
 */

typedef struct lspg_nextshot_struct {
  pthread_mutex_t mutex;	//!< Our mutex for sanity in the multi-threaded program.
  pthread_cond_t  cond;		//!< Condition to wait for a response from our postgresql server.
  int new_value_ready;		//!< Our flag for the condition to wait for.
  int no_rows_returned;		//!< flag indicating that no rows were returned.

  char *dsdir;			//!< Directory for data relative to the ESAF home directory
  int dsdir_isnull;

  char *dspid;			//!< ID string identifying this dataset
  int dspid_isnull;

  double dsowidth;		//!< dataset defined oscillation width
  int dsowidth_isnull;

  char *dsoscaxis;		//!< dataset defined oscillation axis (always omega)
  int dsoscaxis_isnull;

  double dsexp;			//!< dataset defined exposure time
  int dsexp_isnull;

  long long skey;		//!< key identifying a particulary image
  int skey_isnull;

  double sstart;		//!< starting angle
  int sstart_isnull;

  char *sfn;			//!< file name
  int sfn_isnull;

  double dsphi;			//!< dataset defined starting phi angle
  int dsphi_isnull;

  double dsomega;		//!< dataset defined starting omega angle
  int dsomega_isnull;

  double dskappa;		//!< dataset defined starting kappa angle
  int dskappa_isnull;

  double dsdist;		//!< dataset defined detector distance
  int dsdist_isnull;

  double dsnrg;			//!< dataset defined energy
  int dsnrg_isnull;

  unsigned int dshpid;		//!< sample holder ID
  int dshpid_isnull;

  double cx;			//!< centering table x position
  int cx_isnull;

  double cy;			//!< centering table y position
  int cy_isnull;

  double ax;			//!< alignment table x position
  int ax_isnull;

  double ay;			//!< alignment table y position
  int ay_isnull;

  double az;			//!< alignment table z position
  int az_isnull;

  int active;			//!< flag: 1=move to indicated center position, 0=don't move center or alignment tables
  int active_isnull;

  int sindex;			//!< index of frame (used to generate the file extension)
  int sindex_isnull;

  char *stype;			//!< "Normal" or "Gridsearch"
  int stype_isnull;

  double dsowidth2;		//!< next image oscillation width
  int dsowidth2_isnull;

  char *dsoscaxis2;		//!< next image ascillation axis (always "omega")
  int dsoscaxis2_isnull;

  double dsexp2;		//!< next image exposure time
  int dsexp2_isnull;

  double sstart2;		//!< next image start angle
  int sstart2_isnull;

  double dsphi2;		//!< next image phi position
  int dsphi2_isnull;

  double dsomega2;		//!< next image omega position
  int dsomega2_isnull;

  double dskappa2;		//!< next image kappa position
  int dskappa2_isnull;

  double dsdist2;		//!< next image distance
  int dsdist2_isnull;

  double dsnrg2;		//!< next image energy
  int dsnrg2_isnull;

  double cx2;			//!< next image centering table x position
  int cx2_isnull;

  double cy2;			//!< next image centering table y position
  int cy2_isnull;

  double ax2;			//!< next image alignment x position
  int ax2_isnull;

  double ay2;			//!< next image alignment y position
  int ay2_isnull;

  double az2;			//!< next image alignment z position
  int az2_isnull;

  int active2;			//!< flag: 1 if next image should use the above centering parameters
  int active2_isnull;

  int sindex2;			//!< next image index number
  int sindex2_isnull;

  char *stype2;			//!< next image type ("Normal" or "Gridsearch")
  int stype2_isnull;

} lspg_nextshot_t;		//!< definition of the next image to be taken (and the one after that, too!)


extern int pgpmac_use_pg;
extern int pgpmac_use_autoscint;

extern lspg_nextshot_t lspg_nextshot;

extern lspmac_motor_t lspmac_motors[];
extern lspmac_motor_t *omega;
extern lspmac_motor_t *alignx;
extern lspmac_motor_t *aligny;
extern lspmac_motor_t *alignz;
extern lspmac_motor_t *anal;
extern lspmac_motor_t *zoom;
extern lspmac_motor_t *apery;
extern lspmac_motor_t *aperz;
extern lspmac_motor_t *capy;
extern lspmac_motor_t *capz;
extern lspmac_motor_t *scint;
extern lspmac_motor_t *cenx;
extern lspmac_motor_t *ceny;
extern lspmac_motor_t *kappa;
extern lspmac_motor_t *phi;

extern lspmac_motor_t *fshut;
extern lspmac_motor_t *flight;
extern lspmac_motor_t *blight;
extern lspmac_motor_t *fscint;

extern lspmac_motor_t *smart_mag_oo;
extern lspmac_motor_t *blight_ud;
extern lspmac_motor_t *cryo;
extern lspmac_motor_t *dryer;
extern lspmac_motor_t *fluo;
extern lspmac_motor_t *flight_oo;
extern lspmac_motor_t *blight_f;
extern lspmac_motor_t *flight_f;

extern int lspmac_nmotors;

extern lspmac_bi_t    *lp_air;
extern lspmac_bi_t    *hp_air;
extern lspmac_bi_t    *cryo_switch;
extern lspmac_bi_t    *blight_down;
extern lspmac_bi_t    *blight_up;
extern lspmac_bi_t    *cryo_back;
extern lspmac_bi_t    *fluor_back;
extern lspmac_bi_t    *sample_detected;
extern lspmac_bi_t    *etel_ready;
extern lspmac_bi_t    *etel_on;
extern lspmac_bi_t    *etel_init_ok;
extern lspmac_bi_t    *minikappa_ok;
extern lspmac_bi_t    *smart_mag_on;
extern lspmac_bi_t    *arm_parked;
extern lspmac_bi_t    *shutter_open;
extern lspmac_bi_t    *smart_mag_off;
extern lspmac_bi_t    *smart_mag_err;

extern struct timespec omega_zero_time;

extern double lspmac_getPosition( lspmac_motor_t *);

extern WINDOW *term_output;
extern WINDOW *term_input;
extern WINDOW *term_status;
extern WINDOW *term_status2;

extern pthread_mutex_t ncurses_mutex;

extern pthread_cond_t  md2cmds_cond;	
extern pthread_mutex_t md2cmds_mutex;	
extern pthread_cond_t  md2cmds_pg_cond;	
extern pthread_mutex_t md2cmds_pg_mutex;
extern pthread_mutex_t pmac_queue_mutex;
extern pthread_cond_t  pmac_queue_cond;	

extern pthread_mutex_t lspmac_shutter_mutex;
extern pthread_cond_t  lspmac_shutter_cond;
extern int lspmac_shutter_state;
extern int lspmac_shutter_has_opened;
extern pthread_mutex_t lspmac_moving_mutex;
extern pthread_cond_t  lspmac_moving_cond;
extern int lspmac_moving_flags;

extern pthread_mutex_t md2_status_mutex;

#define MD2CMDS_CMD_LENGTH  32
extern char md2cmds_cmd[];			// our command;

extern lsredis_obj_t *md2cmds_md_status_code;

extern char **lspg_array2ptrs( char *);
extern char **lsredis_get_string_array( lsredis_obj_t *p);
extern void lspmac_SockSendDPline( char *, char *fmt, ...);
extern pmac_cmd_queue_t *lspmac_SockSendline(char *, char *, ...);
extern lsredis_obj_t *lsredis_get_obj( char *, ...);
extern char *lsredis_getstr( lsredis_obj_t *p);
extern void PmacSockSendline( char *s);
extern unsigned int lspg_nextsample_all( int *err);
extern char lsredis_getc( lsredis_obj_t *p);
extern long int lsredis_getl( lsredis_obj_t *p);
extern void lsevents_add_listener( char *, void (*cb)(char *));
extern void lsevents_init();
extern void lsevents_remove_listener( char *, void (*cb)(char *));
extern void lsevents_run();
extern void lsevents_send_event( char *, ...);
extern void lsevents_preregister_event( char *fmt, ...);
extern void lslogging_init();
extern void lslogging_log_message( char *fmt, ...);
extern void lslogging_run();
extern void lspg_demandairrights_all();
extern void lspg_getcenter_call();
extern void lspg_getcenter_done();
extern void lspg_getcenter_wait();
extern void lspg_getcurrentsampleid_wait_for_id( unsigned int test);
extern void lspg_init();
extern void lspg_nextshot_call();
extern void lspg_nextshot_done();
extern void lspg_nextshot_wait();
extern void lspg_query_push(void (*cb)( lspg_query_queue_t *, PGresult *), char *fmt, ...);
extern void lspg_run();
extern void lspg_seq_run_prep_all( long long skey, double kappa, double phi, double cx, double cy, double ax, double ay, double az);
extern void lspg_starttransfer_call( unsigned int nextsample, int sample_detected, double ax, double ay, double az, double horz, double vert, double esttime);
extern void lspg_starttransfer_done();
extern void lspg_starttransfer_wait();
extern void lspg_waitcryo_all();
extern void lspg_waitcryo_cb( lspg_query_queue_t *qqp, PGresult *pgr);
extern void lspg_zoom_lut_call();
extern int  lspmac_getBIPosition( lspmac_bi_t *);
extern void lspmac_home1_queue(	lspmac_motor_t *mp);
extern void lspmac_home2_queue(	lspmac_motor_t *mp);
extern void lspmac_abort();
extern void lspmac_init( int, int);
extern int lspmac_jogabs_queue( lspmac_motor_t *, double);
extern int lspmac_move_or_jog_abs_queue( lspmac_motor_t *mp, double requested_position,int use_jo);
extern int lspmac_move_or_jog_preset_queue( lspmac_motor_t *, char *, int);
extern void lspmac_move_or_jog_queue( lspmac_motor_t *, double, int);
extern int lspmac_move_preset_queue( lspmac_motor_t *mp, char *preset_name);
extern int lspmac_moveabs_queue( lspmac_motor_t *, double);
extern int lspmac_jogabs_queue( lspmac_motor_t *, double);
extern int lspmac_moveabs_wait(lspmac_motor_t *mp, double timeout);
extern void lspmac_run();
extern void lspmac_video_rotate( double secs);
extern int  lsredis_cmpnstr( lsredis_obj_t *p, char *s, int n);
extern int  lsredis_cmpstr( lsredis_obj_t *p, char *s);
extern int  lsredis_find_preset( char *base, char *preset_name, double *dval);
extern int  lsredis_getb( lsredis_obj_t *p);
extern double lsredis_getd( lsredis_obj_t *p);
extern void lsredis_init();
extern int  lsredis_regexec( const regex_t *preg, lsredis_obj_t *p, size_t nmatch, regmatch_t *pmatch, int eflags);
extern void lsredis_run();
extern void lsredis_setstr( lsredis_obj_t *p, char *fmt, ...);
extern void lstimer_set_timer( char *, int, unsigned long int, unsigned long int);
extern void lstimer_unset_timer( char *event);
extern void lstimer_init();
extern void lstimer_run();
extern void lsupdate_init();
extern void lsupdate_run();
extern void md2cmds_init();
extern void md2cmds_run();
extern void pgpmac_printf( char *fmt, ...);
extern void lstest_main();
extern int lspmac_est_move_time( double *est_time, int *mmask, lspmac_motor_t *mp_1, int jog_1, char *preset_1, double end_point_1, ...);
extern int lspmac_est_move_time_wait( double move_time, int cmask, lspmac_motor_t *mp_1, ...);
extern void lsredis_set_preset( char *base, char *preset_name, double dval);
extern pthread_mutex_t lsredis_mutex;
extern pthread_cond_t  lsredis_cond;
extern int lsredis_running;
extern lsredis_obj_t *_lsredis_get_obj( char *key);
extern lspmac_motor_t *lspmac_find_motor_by_name( char *name);
extern int lsredis_find_preset_index_by_position( lspmac_motor_t *mp);
void lspmac_SockSendDPControlChar( char *event, char c);
extern int lspmac_set_motion_flags( int *mmaskp, lspmac_motor_t *mp_1, ...);
extern void lsredis_load_presets( char *motor_name);
extern void pgpmac_request_stay_of_execution( int secs);
extern void md2cmds_push_queue( char *action);
extern pmac_cmd_queue_t *lspmac_SockSendControlCharPrint( char *event, char c);
extern void lsredis_config();
