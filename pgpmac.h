/*! \file pgpmac.h
 *  \brief Headers for the entire pgpmac project
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 */
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

//! Number of status box rows
#define LS_DISPLAY_WINDOW_HEIGHT 8


//! Number of status box columns
#define LS_DISPLAY_WINDOW_WIDTH  24

//! Fixed length postgresql query strings.  Queries should all be function calls so this is not as weird as one might think.
#define LS_PG_QUERY_STRING_LENGTH 1024


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
  unsigned char rbuff[1400];			//!< buffer for the returned bytes
  void (*onResponse)(struct lspmac_cmd_queue_struct *,int, unsigned char *);	//!< function to call when response is received.  args are (int fd, nreturned, buffer)
} pmac_cmd_queue_t;

/** Motor information.
 *
 * A catchall for motors and motor like objects.
 * Not all members are used by all objects.
 */
typedef struct lspmac_motor_struct {
  pthread_mutex_t mutex;	//!< coordinate waiting for motor to be done
  pthread_cond_t cond;		//!<
  int lspg_initialized;		//!< bit flags: bit 0 = motor initialized by database, bit 1 = px.kvs value initialized
  void (*read)( struct lspmac_motor_struct *);	//!< function to read the motor status and position
  int not_done;			//!< set to 1 when request is queued, zero after motion has toggled
  int motion_seen;		//!< set to 1 when motion has been verified to have started
  struct lspmac_cmd_queue_struct *pq;	//!< the queue item requesting motion.  Used to check time request was made

  char **home;			//!< pmac commands to home motor
  int homing;			//!< Homing routine started
  int requested_pos_cnts;	//!< requested position
  int *actual_pos_cnts_p;	//!< pointer to the md2_status structure to the actual position
  int actual_pos_cnts;		//!< local copy of actual counts so only our mutex is needed to read
  double position;		//!< scaled position
  double reported_position;	//!< previous position reported to the database
  double requested_position;	//!< The position as requested by the user
  double update_resolution;	//!< Change needs to be at least this big to report as a new position to the database
  char   *update_format;	//!< special format string to create text array for px.kvs update (lsupdate)
  int *status1_p;		//!< First 24 bit PMAC motor status word
  int status1;			//!< local copy of status1
  int *status2_p;		//!< Sectond 24 bit PMAC motor status word
  int status2;			//!< local copy of status2
  char statuss[64];		//!< short text summarizing status
  int motor_num;		//!< pmac motor number
  int coord_num;		//!< coordinate system this motor belongs to (0 if none)
  char *dac_mvar;		//!< controlling mvariable as a string
  char *name;			//!< Name of motor as refered by ls database kvs table
  char *units;			//!< string to use as the units
  char *format;			//!< printf format
  char *write_fmt;		//!< Format string to write requested position to PMAC used for binary io
  int *read_ptr;		//!< With read_mask finds bit to read for binary i/o
  int read_mask;		//!< WIth read_ptr find bit to read for binary i/o
  void (*moveAbs)( struct lspmac_motor_struct *, double);	//!< function to move the motor
  double u2c;			//!< conversion from counts to units: 0.0 means not loaded yet
  double *lut;			//!< lookup table (instead of u2c)
  int  nlut;			//!< length of lut
  double max_speed;		//!< our maximum speed (cts/msec)
  double max_accel;		//!< our maximum acceleration (cts/msec^2)
  WINDOW *win;			//!< our ncurses window
} lspmac_motor_t;


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
extern lspmac_motor_t *scinz;
extern lspmac_motor_t *cenx;
extern lspmac_motor_t *ceny;
extern lspmac_motor_t *kappa;
extern lspmac_motor_t *phi;

extern lspmac_motor_t *fshut;
extern lspmac_motor_t *flight;
extern lspmac_motor_t *blight;
extern lspmac_motor_t *fscint;

extern lspmac_motor_t *blight_ud;

extern int lspmac_nmotors;

extern WINDOW *term_output;
extern WINDOW *term_input;
extern WINDOW *term_status;
extern WINDOW *term_status2;

extern pthread_mutex_t ncurses_mutex;

extern pthread_cond_t  md2cmds_cond;		// condition to signal when it's time to run an md2 command
extern pthread_mutex_t md2cmds_mutex;		// mutex for the condition
extern pthread_cond_t  md2cmds_pg_cond;		// coordinate call and response
extern pthread_mutex_t md2cmds_pg_mutex;	// message passing between md2cmds and pg

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


extern void PmacSockSendline( char *s);
extern void lspg_seq_run_prep_all( long long skey, double kappa, double phi, double cx, double cy, double ax, double ay, double az);
extern void lspg_zoom_lut_call();
extern void pgpmac_printf( char *fmt, ...);
extern void lspmac_init( int, int);
extern void lspg_init();
extern void lsupdate_init();
extern void md2cmds_init();
extern void lspmac_run();
extern void lspg_run();
extern void lsupdate_run();
extern void md2cmds_run();
