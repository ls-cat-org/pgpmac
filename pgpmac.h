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

#define LS_DISPLAY_WINDOW_HEIGHT 8
#define LS_DISPLAY_WINDOW_WIDTH  24
#define LS_PG_QUERY_STRING_LENGTH 1024


typedef struct lspmac_motor_struct {
  int raw_position;		// raw position read from dpram
  int dpram_position_offset;	// offset of our position in the md2_status buffer ready by getmem
  int status1;
  int status1_offset;		// offset in md2_status for the status 1 24 bits
  int status2;
  int status2_offset;		// offset in md2_status for the status 2 24 bits
  int motor_num;		// pmac motor number
  char *units;			// string to use as the units
  char *format;			// printf format
  double u2c;			// conversion from counts to units
  WINDOW *win;			// our ncurses window
} lspmac_motor_t;

typedef struct lspg_nextshot_struct {
  pthread_mutex_t mutex;
  pthread_cond_t  cond;
  int new_value_ready;

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
} lspg_nextshot_t;
extern lspg_nextshot_t lspg_nextshot;

extern lspmac_motor_t lspmac_motors[];
extern int lspmac_nmotors;

extern WINDOW *term_output;
extern WINDOW *term_input;
extern WINDOW *term_status;

extern pthread_mutex_t ncurses_mutex;

extern pthread_cond_t  md2cmds_cond;		// condition to signal when it's time to run an md2 command
extern pthread_mutex_t md2cmds_mutex;		// mutex for the condition
extern pthread_cond_t  md2cmds_pg_cond;		// coordinate call and response
extern pthread_mutex_t md2cmds_pg_mutex;	// message passing between md2cmds and pg



#define MD2CMDS_CMD_LENGTH  32
extern char md2cmds_cmd[];			// our command;


void PmacSockSendline( char *s);
