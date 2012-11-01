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

#define LS_DISPLAY_WINDOW_HEIGHT 8
#define LS_DISPLAY_WINDOW_WIDTH  24
#define LS_PG_QUERY_STRING_LENGTH 1024


typedef unsigned char BYTE;
typedef unsigned short WORD;

typedef struct tagEthernetCmd {
  unsigned char  RequestType;
  unsigned char  Request;
  unsigned short wValue;
  unsigned short wIndex;
  unsigned short wLength;
  unsigned char  bData[1492];
} pmac_cmd_t;

typedef struct lspmac_cmd_queue_struct {
  pmac_cmd_t pcmd;				// the pmac command to send
  int no_reply;					// 1 = no reply is expected, 0 = expect a reply
  struct timespec time_sent;			// time this item was dequeued and sent to the pmac
  unsigned char rbuff[1400];			// buffer for the returned bytes
  void (*onResponse)(struct lspmac_cmd_queue_struct *,int, unsigned char *);	// function to call when response is received.  args are (int fd, nreturned, buffer)
} pmac_cmd_queue_t;

typedef struct lspmac_motor_struct {
  pthread_mutex_t mutex;	// coordinate waiting for motor to be done
  pthread_cond_t cond;		//
  void (*read)( struct lspmac_motor_struct *);	// function to read the motor status and position
  int not_done;			// set to 1 when request is queued, zero after motion has toggled
  int motion_seen;		// set to 1 when motion has been verified to have started
  struct lspmac_cmd_queue_struct *pq;	// the queue item requesting motion.  Used to check time request was made

  int requested_pos_cnts;	// requested position
  int *actual_pos_cnts_p;	// pointer to the md2_status structure to the actual position
  double position;		// scaled position
  double reported_position;	// previous position reported to the database
  double requested_position;	// The position as requested by the user
  double update_resolution;	// Change needs to be at least this big to report as a new position to the database
  int *status1;
  int *status2;
  int motor_num;		// pmac motor number
  char *dac_mvar;		// controlling mvariable as a string
  char *name;			// Name of motor as refered by ls database kvs table
  char *units;			// string to use as the units
  char *format;			// printf format
  char *write_fmt;		// 
  int *read_ptr;
  int read_mask;
  void (*moveAbs)( struct lspmac_motor_struct *, double);	// function to move the motor
  double u2c;			// conversion from counts to units: 0.0 means not loaded yet
  double *lut;			// lookup table (instead of u2c)
  int  nlut;			// length of lut
  double max_speed;		// our maximum speed (cts/msec)
  double max_accel;		// our maximum acceleration (cts/msec^2)
  WINDOW *win;			// our ncurses window
} lspmac_motor_t;

typedef struct lspg_nextshot_struct {
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  int new_value_ready;
  int no_rows_returned;

  char *dsdir;
  int dsdir_isnull;

  char *dspid;
  int dspid_isnull;

  double dsowidth;
  int dsowidth_isnull;

  char *dsoscaxis;
  int dsoscaxis_isnull;

  double dsexp;
  int dsexp_isnull;

  long long skey;
  int skey_isnull;

  double sstart;
  int sstart_isnull;

  char *sfn;
  int sfn_isnull;

  double dsphi;
  int dsphi_isnull;

  double dsomega;
  int dsomega_isnull;

  double dskappa;
  int dskappa_isnull;

  double dsdist;
  int dsdist_isnull;

  double dsnrg;
  int dsnrg_isnull;

  unsigned int dshpid;
  int dshpid_isnull;

  double cx;
  int cx_isnull;

  double cy;
  int cy_isnull;

  double ax;
  int ax_isnull;

  double ay;
  int ay_isnull;

  double az;
  int az_isnull;

  int active;
  int active_isnull;

  int sindex;
  int sindex_isnull;

  char *stype;
  int stype_isnull;

  double dsowidth2;
  int dsowidth2_isnull;

  char *dsoscaxis2;
  int dsoscaxis2_isnull;

  double dsexp2;
  int dsexp2_isnull;

  double sstart2;
  int sstart2_isnull;

  double dsphi2;
  int dsphi2_isnull;

  double dsomega2;
  int dsomega2_isnull;

  double dskappa2;
  int dskappa2_isnull;

  double dsdist2;
  int dsdist2_isnull;

  double dsnrg2;
  int dsnrg2_isnull;

  double cx2;
  int cx2_isnull;

  double cy2;
  int cy2_isnull;

  double ax2;
  int ax2_isnull;

  double ay2;
  int ay2_isnull;

  double az2;
  int az2_isnull;

  int active2;
  int active2_isnull;

  int sindex2;
  int sindex2_isnull;

  char *stype2;
  int stype2_isnull;

} lspg_nextshot_t;
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

extern lspmac_motor_t *blight_up;

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