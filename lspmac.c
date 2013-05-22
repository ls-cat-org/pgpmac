#include "pgpmac.h"
/*! \file lspmac.c
 *  \brief Routines concerned with communication with PMAC.
 *  \date 2012 – 2013
 *  \author Keith Brister
 *  \copyright All Rights Reserved

  This is a state machine (surprise!)

  Lacking is support for writingbuffer, control writing and reading,
  as well as double buffered memory It looks like several different
  methods of managing PMAC communications are possible.  Here is set
  up a queue of outgoing commands and deal completely with the result
  before sending the next.  A full handshake of acknowledgements and
  "readready" is expected.

  Most of these states are to deal with the "serial-port" style of
  communications.  Things are surprisingly simple for the double
  buffer ascii and control character methods.


<pre>

 State    Description
  -1      Reset the connection
   0      Detached: need to connect to tcp port
   1	  Idle (waiting for a command to send to the pmac)
   2	  Send command
   3	  Waiting for command acknowledgement (no further response expected)
   4	  Waiting for control character acknowledgement (further response expected)
   5	  Waiting for command acknowledgement (further response expected)
   6	  Waiting for get memory response
   7	  Send controlresponse
   8	  Send readready
   9	  Waiting for acknowledgement of "readready"
  10	  Send readbuffer
  11	  Waiting for control response
  12	  Waiting for readbuffer response

</pre>
*/


#define LS_PMAC_STATE_RESET	-1
#define LS_PMAC_STATE_DETACHED  0
#define LS_PMAC_STATE_IDLE	1
#define LS_PMAC_STATE_SC	2
#define LS_PMAC_STATE_WACK_NFR	3
#define LS_PMAC_STATE_WACK_CC	4
#define LS_PMAC_STATE_WACK	5
#define LS_PMAC_STATE_GMR	6
#define LS_PMAC_STATE_CR	7
#define LS_PMAC_STATE_RR	8
#define LS_PMAC_STATE_WACK_RR	9
#define LS_PMAC_STATE_GB       10
#define LS_PMAC_STATE_WCR      11
#define LS_PMAC_STATE_WGB      12
static int ls_pmac_state = LS_PMAC_STATE_DETACHED;	//!< Current state of the PMAC communications state machine

//#define SHOW_RATE

//static lsredis_obj_t *lspmac_md2_init;

void lspmac_get_ascii( char *);			//!< Forward declarateion

static int lspmac_running = 1;			//!< exit worker thread when zero
int lspmac_shutter_state;			//!< State of the shutter, used to detect changes
int lspmac_shutter_has_opened;			//!< Indicates that the shutter had opened, perhaps briefly even if the state did not change
pthread_mutex_t lspmac_shutter_mutex;		//!< Coordinates threads reading shutter status
pthread_cond_t  lspmac_shutter_cond;		//!< Allows waiting for the shutter status to change
pthread_mutex_t lspmac_moving_mutex;		//!< Coordinate moving motors between threads
pthread_cond_t  lspmac_moving_cond;		//!< Wait for motor(s) to finish moving condition
int lspmac_moving_flags;			//!< Flag used to implement motor moving condition

static uint16_t lspmac_control_char = 0;	//!< The control character we've sent

static pthread_mutex_t lspmac_ascii_mutex;	//!< Keep too many processes from sending commands at once
static int lspmac_ascii_busy = 0;		//!< flag for condition to wait for

static int omega_zero_search = 0;		//!< Indicate we'd really like to know when omega crosses zero
static double omega_zero_velocity = 0;		//!< rate (cnts/sec) that omega was traveling when it crossed zero
struct timespec omega_zero_time;		//!< Time we believe that omega crossed zero
static struct timespec lspmac_status_time;	//!< Time the status was read
static struct timespec lspmac_status_last_time;	//!< Time the status was read

static pthread_t pmac_thread;			//!< our thread to manage access and communication to the pmac
pthread_mutex_t pmac_queue_mutex;		//!< manage access to the pmac command queue
pthread_cond_t  pmac_queue_cond;		//!< wait for a command to be sent to PMAC before continuing
static struct pollfd pmacfd;		        //!< our poll structure

static int getivars = 0;			//!< flag set at initialization to send i vars to db
static int getmvars = 0;			//!< flag set at initialization to send m vars to db

lspmac_bi_t lspmac_bis[32];			//!< array of binary inputs
int lspmac_nbis = 0;				//!< number of active binary inputs

#define LSPMAC_MAX_MOTORS 48
lspmac_motor_t lspmac_motors[LSPMAC_MAX_MOTORS];//!< All our motors
int lspmac_nmotors = 0;				//!< The number of motors we manage
struct hsearch_data motors_ht;			//!< A hash table to find motors by name

lspmac_motor_t *omega;				//!< MD2 omega axis (the air bearing)
lspmac_motor_t *alignx;				//!< Alignment stage X
lspmac_motor_t *aligny;				//!< Alignment stage Y
lspmac_motor_t *alignz;				//!< Alignment stage X
lspmac_motor_t *anal;				//!< Polaroid analyzer motor
lspmac_motor_t *zoom;				//!< Optical zoom
lspmac_motor_t *apery;				//!< Aperture Y
lspmac_motor_t *aperz;				//!< Aperture Z
lspmac_motor_t *capy;				//!< Capillary Y
lspmac_motor_t *capz;				//!< Capillary Z
lspmac_motor_t *scint;				//!< Scintillator Z
lspmac_motor_t *cenx;				//!< Centering Table X
lspmac_motor_t *ceny;				//!< Centering Table Y
lspmac_motor_t *kappa;				//!< Kappa
lspmac_motor_t *phi;				//!< Phi (not data collection axis)

lspmac_motor_t *fshut;				//!< Fast shutter
lspmac_motor_t *flight;				//!< Front Light DAC
lspmac_motor_t *blight;				//!< Back Light DAC
lspmac_motor_t *fscint;				//!< Scintillator Piezo DAC

lspmac_motor_t *smart_mag_oo;			//!< Smart Magnet on/off
lspmac_motor_t *blight_ud;			//!< Back light Up/Down actuator
lspmac_motor_t *cryo;				//!< Move the cryostream towards or away from the crystal
lspmac_motor_t *dryer;				//!< blow air on the scintilator to dry it off
lspmac_motor_t *fluo;				//!< Move the fluorescence detector in/out
lspmac_motor_t *flight_oo;			//!< Turn front light on/off
lspmac_motor_t *blight_f;			//!< Back light scale factor
lspmac_motor_t *flight_f;			//!< Front light scale factor


lspmac_bi_t    *lp_air;				//!< Low pressure air OK
lspmac_bi_t    *hp_air;				//!< High pressure air OK
lspmac_bi_t    *cryo_switch;			//!< that little toggle switch for the cryo
lspmac_bi_t    *blight_down;			//!< Backlight is down
lspmac_bi_t    *blight_up;			//!< Backlight is up
lspmac_bi_t    *cryo_back;			//!< cryo is in the back position
lspmac_bi_t    *fluor_back;			//!< fluor is in the back position
lspmac_bi_t    *sample_detected;		//!< smart magnet detected sample
lspmac_bi_t    *etel_ready;			//!< ETEL is ready
lspmac_bi_t    *etel_on;			//!< ETEL is on
lspmac_bi_t    *etel_init_ok;			//!< ETEL initialized OK
lspmac_bi_t    *minikappa_ok;			//!< Minikappa is OK (whatever that means)
lspmac_bi_t    *smart_mag_on;			//!< smart magnet is on
lspmac_bi_t    *arm_parked;			//!< (whose arm?  parked where?)
lspmac_bi_t    *shutter_open;			//!< shutter is open (note in pmc says this is a slow input)
lspmac_bi_t    *smart_mag_err;			//!< smart magnet error (coil broken perhaps)
lspmac_bi_t    *smart_mag_off;			//!< smart magnet is off


//! Regex to pick out preset name and corresponding position
#define LSPMAC_PRESET_REGEX "(.*\\.%s\\.presets)\\.([0-9]+)\\.(name|position)"


//! The PMAC (only) listens on this port
#define PMACPORT 1025

//! PMAC command size in bytes.

// This size does not include the data, and hence, is fixed.
//
#define pmac_cmd_size 8

#define VR_UPLOAD		0xc0
#define VR_DOWNLOAD		0x40

#define VR_PMAC_SENDLINE	0xb0
#define VR_PMAC_GETLINE		0xb1
#define VR_PMAC_FLUSH		0xb3
#define VR_PMAC_GETMEM		0xb4
#define VR_PMAC_SETMEM		0xb5
#define VR_PMAC_SENDCTRLCHAR	0xb6
#define VR_PMAC_SETBIT		0xba
#define VR_PMAC_SETBITS		0xbb
#define VR_PMAC_PORT		0xbe
#define VR_PMAC_GETRESPONSE	0xbf
#define VR_PMAC_READREADY	0xc2
#define VR_CTRL_RESPONSE	0xc4
#define VR_PMAC_GETBUFFER	0xc5
#define VR_PMAC_WRITEBUFFER	0xc6
#define VR_PMAC_WRITEERROR	0xc7
#define VR_FWDOWNLOAD		0xcb
#define VR_IPADDRESS		0xe0


static unsigned char dbmem[64*1024];		//!< double buffered memory
static int dbmemIn = 0;				//!< next location

//! Minimum time between commands to the pmac
//
//#define PMAC_MIN_CMD_TIME 40000.0
#define PMAC_MIN_CMD_TIME 10000.0
static struct timeval pmac_time_sent, now;	//!< used to ensure we do not send commands to the pmac too often.  Only needed for non-DB commands.

//! Size of the PMAC command queue.
#define PMAC_CMD_QUEUE_LENGTH 2048
static pmac_cmd_t rr_cmd, gb_cmd, cr_cmd;		//!< commands to send out "readready", "getbuffer", "controlresponse" (initialized in main)
static pmac_cmd_queue_t ethCmdQueue[PMAC_CMD_QUEUE_LENGTH];	//!< PMAC command queue
static unsigned int ethCmdOn    = 0;				//!< points to next empty PMAC command queue position
static unsigned int ethCmdOff   = 0;				//!< points to current command (or none if == ethCmdOn)
static unsigned int ethCmdReply = 0;				//!< Used like ethCmdOff only to deal with the pmac reply to a command

//! Decode the errors perhaps returned by the PMAC
static char *pmac_error_strs[] = {
  "ERR000: Unknown error",
  "ERR001: Command not allowed during program execution",
  "ERR002: Password error",
  "ERR003: Data error or unrecognized command",
  "ERR004: Illegal character",
  "ERR005: Command not allowed unless buffer is open",
  "ERR006: No room in buffer for command",
  "ERR007: Buffer already in use",
  "ERR008: MACRO auziliary communication error",
  "ERR009: Program structure error (e.g. ENDIF without IF)",
  "ERR010: Both overtravel limits set for a motor in the C.S.",
  "ERR011: Previous move not completed",
  "ERR012: A motor in the coordinate system is open-loop",
  "ERR013: A motor in the coordinate system is not activated",
  "ERR014: No motors in the coordinate system",
  "ERR015: Not pointer to valid program buffer",
  "ERR016: Running improperly structure program (e.g. missing ENDWHILE)",
  "ERR017: Trying to resume after H or Q with motors out of stopped position",
  "ERR018: Attempt to perform phase reference during move, move during phase reference, or enabling with phase clock error",
  "ERR019: Illegal position-chage command while moves stored in CCBUFFER",
  "ERR020: FSAVE issued on Turbo PMAC with incompatible flash memory",
  "ERR021: FSAVE issued while clearing old flash memory sector",
  "ERR022: FREAD attempted but the flash memory is bad"
};


//! The block of memory retrieved in a status request.

//
// DPRAM is from $60000 to $60FFF or $603FFF
//
// We can quickly read 1400 bytes = 350 32-bit registers
// so reading $60000 to 60015D is not too expensive
//
//  $060000  Control Panel Functions
//  $06001A  Motor Data Reporting Buffer
//  $06019D  Background Data Reporting Buffer
//  $0603A7  DPRAM ASCII Command Buffer
//  $0603D0  DPRAM ASCII Response Buffer
//  $060411  Background Variable Read Buffer Control
//  $060413  Binary Rotary Buffer Control
//  $06044F  DPRAM Data Gathering Buffer Control
//  $060450  Variable-Sized Buffers and Open-Use Space
//  $060FFF  End of Small (8k X 16) DPRAM
//  $063FFF  End of Large (32k X 16) DPRAM
//
//
// Gather data starts at $600450 (per turbo pmac user manual)
//



typedef  struct md2StatusStruct {
  //
  // Pmac stores data in 24 bit or 48 bit words
  //
  // The DP: addresses point to the low 16 bits of the 24 bit X and Y registers
  //
  //  PMAC       Our dp ram offset
  //
  // Y:$060000      0x0000
  // X:$060000      0x0002
  // Y:$060001      0x0004
  // X:$060001      0x0006
  // And so forth
  //
  //
  int dummy1;			// 0x000		$60100 
  int omega_status_1;		// 0x004		$60101 
  int alignx_status_1;		// 0x008		$60102
  int aligny_status_1;		// 0x00C		$60103
  int alignz_status_1;		// 0x010		$60104
  int analyzer_status_1;	// 0x014		$60105
  int zoom_status_1;		// 0x018		$60106
  int aperturey_status_1;	// 0x01C		$60107
  int aperturez_status_1;	// 0x020		$60108
  int capy_status_1;		// 0x024		$60109
  int capz_status_1;		// 0x028		$6010A
  int scint_status_1;		// 0x02C		$6010B
  int centerx_status_1;		// 0x030		$6010C
  int centery_status_1;		// 0x034		$6010D
  int kappa_status_1;		// 0x038		$6010E
  int phi_status_1;		// 0x03C		$6010F

  int dummy2;			// 0x040		$60110
  int omega_status_2;		// 0x044		$60111
  int alignx_status_2;		// 0x048		$60112
  int aligny_status_2;		// 0x04C		$60113
  int alignz_status_2;		// 0x050		$60114
  int analyzer_status_2;	// 0x054		$60115
  int zoom_status_2;		// 0x058		$60116
  int aperturey_status_2;	// 0x05C		$60117
  int aperturez_status_2;	// 0x060		$60118
  int capy_status_2;		// 0x064		$60119
  int capz_status_2;		// 0x068		$6011A
  int scint_status_2;		// 0x06C		$6011B
  int centerx_status_2;		// 0x070		$6011C
  int centery_status_2;		// 0x074		$6011D
  int kappa_status_2;		// 0x078		$6011E
  int phi_status_2;		// 0x07C		$6011F

  int dummy3;			// 0x080		$60120
  int omega_act_pos;		// 0x084		$60121
  int alignx_act_pos;		// 0x088		$60122
  int aligny_act_pos;		// 0x08C		$60123
  int alignz_act_pos;		// 0x090		$60124
  int analyzer_act_pos;		// 0x094		$60125
  int zoom_act_pos;		// 0x098		$60126
  int aperturey_act_pos;	// 0x09C		$60127
  int aperturez_act_pos;	// 0x0A0		$60128
  int capy_act_pos;		// 0x0A4		$60129
  int capz_act_pos;		// 0x0A8		$6012A
  int scint_act_pos;		// 0x0AC		$6012B
  int centerx_act_pos;		// 0x0B0		$6012C
  int centery_act_pos;		// 0x0B4		$6012D
  int kappa_act_pos;		// 0x0B8		$6012E
  int phi_act_pos;		// 0x0BC		$6012F


  int acc11c_1;			// 0x0C0		$60130
  int acc11c_2;			// 0x0C4		$60131
  int acc11c_3;			// 0x0C8		$60132
  int acc11c_5;			// 0x0CC		$60133
  int acc11c_6;			// 0x0D0		$60134
  int front_dac;		// 0x0D4		$60135
  int back_dac;			// 0x0D8		$60136
  int scint_piezo;		// 0x0DC		$60137

  int dummy4;			// 0x0E0		$60138
  int dummy5;			// 0x0E4		$60139
  int dummy6;			// 0x0E8		$6013A
  int dummy7;			// 0x0EC		$6013B
  int dummy8;			// 0x0F0		$6013C
  int dummy9;			// 0x0F4		$6013D
  int dummyA;			// 0x0F8		$6013E
  int dummyB;			// 0x0FC		$6013F

  int fs_is_open;		// 0x100		$60140
  int phiscan;			// 0x104		$60141
  int fs_has_opened;		// 0x108		$60142
  int fs_has_opened_globally;	// 0x10C		$60143
  int number_passes;		// 0x110		$60144

  int moving_flags;		// 0x114		$60145

} md2_status_t;

static md2_status_t md2_status;		//!< Buffer for MD2 Status
pthread_mutex_t md2_status_mutex;	//!< Synchronize reading/writting status buffer


typedef struct lspmac_ascii_buffers_struct {
  //                               here		DPRAM		PMAC
  uint16_t command_buf;		// 0x000	$0E9C		$0603A7
  uint16_t command_buf_cc;	// 0x002	$0E9E		
  char command_str[160];	// 0x004	$0EA0		$0603A8
  uint16_t response_buf;	// 0x0A4	$0F40		$0603D0
  uint16_t response_n;		// 0x0A6	$0F42
  char response_str[256];	// 0x0A8	$0F44		$0603D1
				// 0x1A8	$1044		$060411
} lspmac_ascii_buffers_t;
static lspmac_ascii_buffers_t lspmac_ascii_buffers;
pthread_mutex_t lspmac_ascii_buffers_mutex;

#define LSPMAC_DPASCII_QUEUE_LENGTH 1024
typedef struct lspmac_dpascii_queue_struct {
  char *event;			// pointer to location that wants the cmd_queue pointer
  char pl[160];			// Our payload
} lspmac_dpascii_queue_t;

static lspmac_dpascii_queue_t lspmac_dpascii_queue[LSPMAC_DPASCII_QUEUE_LENGTH];
static uint32_t lspmac_dpascii_on  = 0;
static uint32_t lspmac_dpascii_off = 0;

typedef struct lspmac_combined_move_struct {
  int    Delta;			// change from curent position in counts
  int    moveme;		// flag: non-zero means move this motor
  int    coord_num;		// Our coordinate system
  int    axis;			// The axes to move
} lspmac_combined_move_t;


/** Look up table support for motor positions (think x=zoom, y=light intensity)
 * use a lookup table to find the "counts" to move the motor to the requested position
 * The look up table is a simple one dimensional array with the x values as even indicies
 * and the y values as odd indices
 *
 * Returns: y value
 *
 */

double lspmac_lut(
		  int nlut,		/**< [in] number of entries in lookup table					*/
		  double *lut,		/**< [in] The lookup table: even indicies are the x values, odd are the y's	*/
		  double x		/**< [in] The x value we are looking up.					*/
		  ) {
  int i, foundone;
  double m;
  double y1, y2, x1, x2, y;

  foundone = 0;
  if( lut != NULL && nlut > 1) {
    for( i=0; i < 2*nlut; i += 2) {
      x1 = lut[i];
      y1 = lut[i+1];
      if( i < 2*nlut - 2) {
	x2 = lut[i+2];
	y2 = lut[i+3];
      }

      //
      // First one too big?  Use the y value of the first element
      //
      if( i == 0 && x1 > x) {
	y = y1;
	foundone = 1;
	break;
      }

      //
      // Look for equality
      //
      if( x1 == x) {
	y = y1;
	foundone = 1;
	break;
      }

      //
      // Maybe interpolate
      //
      if( (i < 2*nlut-2) && x < x2) {
	m = (y2 - y1) / (x2 - x1);
	y = m*(x - x1) + y1;
	foundone = 1;
	break;
      }
    }
    if( foundone == 0) {
      // must be bigger than the last entry
      //
      //
      y = lut[2*(nlut-1) + 1];
    }
    return y;
  }
  return 0.0;
}

double lspmac_rlut(
		   int nlut,		/**< [in] number of entries in lookup table					*/
		   double *lut,		/**< [in] our lookup table							*/
		   double y		/**< [in] the y value for which we need an x					*/
		   ) {
  int i, foundone, up;
  double m;
  double y1, y2, x1, x2, x;

  foundone = 0;
  if( lut != NULL && nlut > 1) {

    //
    // are the table values going up or down?
    //
    if( lut[1] < lut[2*nlut-1])
      up = 1;
    else
      up = 0;

    //
    // Linear search
    //
    for( i=0; i < 2*nlut; i += 2) {
      x1 = lut[i];
      y1 = lut[i+1];
      if( i < 2*nlut - 2) {
	x2 = lut[i+2];
	y2 = lut[i+3];
      }
      //
      // see if y is before the beginning of the table
      //
      if( i==0 && ( up ? y1 > y : y1 < y)) {
	x = x1;
	foundone = 1;
	break;
      }
      //
      // Did we, perhaps, nail it?
      //
      if( y1 == y) {
	x = x1;
	foundone = 1;
	break;
      }

      //
      // Interpolate between the two values (if we've not bumped our heads on the end of the table)
      //
      if( (i < 2*nlut-2) && (up ? y < y2 : y > y2)) {
	m = (x2 - x1) / (y2 - y1);
	x = m * (y - y1) + x1;
	foundone = 1;
	break;
      }
    }
    //
    // y is off the charts: just use the last value
    //
    if( foundone == 0 ) {
      x = lut[2*(nlut-1)];
    }
    return x;
  }
  return 0.0;
}



/** Prints a hex dump of the given data.
 *  Used to debug packet data.
 */

void hex_dump(
	      int n,		/**< [in] Number of bytes passed in s	*/
	      unsigned char *s		/**< [in] Data to dump			*/
	      ) {

  int i;	// row counter
  int j;	// column counter
  unsigned char outs[128], outs1[4];

  for( i=0; n > 0; i++) {

    sprintf( (char *)outs, "%04d: ", 16*i);
    for( j=0; j<16 && n > 0; j++) {
      if( j==8)
	strcat( (char *)outs, "  ");
      sprintf( (char *)outs1, " %02x",  *(s + 16*i + j));
      strcat( (char *)outs, (char *)outs1);
      n--;
    }
    lslogging_log_message( "hex_dump: %s", outs);
  }
}

/** Replace \\r with \\n in null terminated string and print result to terminal.
 * Needed to turn PMAC messages into something printable.
 */

char *cleanstr(
	      char *s	/**< [in] String to print to terminal.	*/
	      ) {
  char t[256];
  int i;

  t[0] = 0;
  for( i=0; i<strlen( s) && i < sizeof( t); i++) {
    if( s[i] == '\r')
      t[i] = '\n';
    else
      t[i] = s[i];
  }
  t[i] = 0;
  return strdup( s);
}

/** Connect to the PMAC socket.
 *  Establish or reestablish communications.
 */

void lsConnect(
	       char *ipaddr	/**< [in] String representation of the IP address (dot quad or FQN)	*/
	       ) {
  int psock;			// our socket: value stored in pmacfda.fd
  int err;			// error code from some system calls
  struct sockaddr_in *addrP;	// our address structure to connect to
  struct addrinfo ai_hints;	// required for getaddrinfo
  struct addrinfo *ai_resultP;	// linked list of address structures (we'll always pick the first)

  pmacfd.fd     = -1;
  pmacfd.events = 0;

  // Initial buffer(s)
  memset( &ai_hints,  0, sizeof( ai_hints));

  ai_hints.ai_family   = AF_INET;
  ai_hints.ai_socktype = SOCK_STREAM;


  //
  // get address
  //
  err = getaddrinfo( ipaddr, NULL, &ai_hints, &ai_resultP);
  if( err != 0) {

    lslogging_log_message( "Could not find address: %s", gai_strerror( err));

    return;
  }


  addrP = (struct sockaddr_in *)ai_resultP->ai_addr;
  addrP->sin_port = htons( PMACPORT);


  psock = socket( PF_INET, SOCK_STREAM, 0);
  if( psock == -1) {
    lslogging_log_message( "Could not create socket");
    return;
  }

  err = connect( psock, (const struct sockaddr *)addrP, sizeof( *addrP));
  if( err != 0) {
    lslogging_log_message( "Could not connect socket: %s", strerror( errno));
    return;
  }

  ls_pmac_state = LS_PMAC_STATE_IDLE;
  pmacfd.fd     = psock;
  pmacfd.events = POLLIN;

}



/** Clear the queue as part of PMAC reinitialization
 */
void lspmac_reset_queue() {
  pthread_mutex_lock( &pmac_queue_mutex);
  ethCmdOn    = 0;
  ethCmdOff   = 0;
  ethCmdReply = 0;
  pthread_mutex_unlock( &pmac_queue_mutex);
}

/** Put a new command on the queue.
 *
 * Pointer is returned so caller can evaluate the time command was actually sent.
 */

pmac_cmd_queue_t *lspmac_push_queue(
				    pmac_cmd_queue_t *cmd	/**< Command to send to the PMAC	*/
				    ) {
  pmac_cmd_queue_t *rtn;

  pthread_mutex_lock( &pmac_queue_mutex);
  rtn = &(ethCmdQueue[(ethCmdOn++) % PMAC_CMD_QUEUE_LENGTH]);
  memcpy( rtn, cmd, sizeof( pmac_cmd_queue_t));
  rtn->time_sent.tv_sec  = 0;
  rtn->time_sent.tv_nsec = 0;
  pthread_cond_signal( &pmac_queue_cond);
  pthread_mutex_unlock( &pmac_queue_mutex);

  return rtn;
}

/** Remove the oldest queue item.
 *  Used to send command to PMAC.
 *  Note that there is a separate reply index
 *  to ensure we've know to what command a reply
 *  is refering.
 *  Returns the item.
 */

pmac_cmd_queue_t *lspmac_pop_queue() {
  pmac_cmd_queue_t *rtn;

  pthread_mutex_lock( &pmac_queue_mutex);

  if( ethCmdOn == ethCmdOff)
    rtn = NULL;
  else {
    rtn = &(ethCmdQueue[(ethCmdOff++) % PMAC_CMD_QUEUE_LENGTH]);
    clock_gettime( CLOCK_REALTIME, &(rtn->time_sent));
  }
  pthread_mutex_unlock( &pmac_queue_mutex);
  return rtn;
}


/** Remove the next command queue item that is waiting for a reply.
 *  We always need a reply to know we are done with a given command.
 *  Returns the item.
 */
pmac_cmd_queue_t *lspmac_pop_reply() {
  pmac_cmd_queue_t *rtn;

  pthread_mutex_lock( &pmac_queue_mutex);

  if( ethCmdOn == ethCmdReply)
    rtn = NULL;
  else
    rtn = &(ethCmdQueue[(ethCmdReply++) % PMAC_CMD_QUEUE_LENGTH]);

  pthread_mutex_unlock( &pmac_queue_mutex);
  return rtn;
}

/** Compose a packet and send it to the PMAC.
 *  This is the meat of the PMAC communications routines.
 *  The queued command is returned.
 */
pmac_cmd_queue_t *lspmac_send_command(
				      int rqType,		/**< [in] VR_UPLOAD or VR_DOWNLOAD			*/
				      int rq,			/**< [in] PMAC command (see PMAC User Manual		*/
				      int wValue,		/**< [in] Command argument 1				*/
				      int wIndex,		/**< [in] Command argument 2				*/
				      int wLength,		/**< [in] Length of data array				*/
				      char *data,		/**< [in] Data array (or NULL)				*/
				      void (*responseCB)(pmac_cmd_queue_t *, int, char *),
				      /**< [in] Function to call when a response is read from the PMAC			*/
				      int no_reply,		/**< [in] Flag, non-zero means no reply is expected	*/
				      char *event		/**< [in] base name for events				*/
				      ) {
  static pmac_cmd_queue_t cmd;

  cmd.pcmd.RequestType = rqType;
  cmd.pcmd.Request     = rq;
  cmd.pcmd.wValue      = htons(wValue);
  cmd.pcmd.wIndex      = htons(wIndex);
  cmd.pcmd.wLength     = htons(wLength);
  cmd.onResponse       = responseCB;
  cmd.no_reply	       = no_reply;
  cmd.event            = event;

  //
  // Setting the message buff bData requires a bit more care to avoid over filling it
  // or sending garbage in the unused bytes.
  //

  if( wLength > sizeof( cmd.pcmd.bData)) {
    //
    // Bad things happen if we do not catch this case.
    //
    lslogging_log_message( "Message Length %d longer than maximum of %ld, aborting", wLength, sizeof( cmd.pcmd.bData));
    exit( -1);
  }
  if( data == NULL) {
    memset( cmd.pcmd.bData, 0, sizeof( cmd.pcmd.bData));
  } else {
    //
    // This could leave bData non-null terminated.  I do not know if this is a problem.
    //
    if( wLength > 0)
      memcpy( cmd.pcmd.bData, data, wLength);
    if( wLength < sizeof( cmd.pcmd.bData))
      memset( cmd.pcmd.bData + wLength, 0, sizeof( cmd.pcmd.bData) - wLength);
  }

  return lspmac_push_queue( &cmd);
}


/** Reset the PMAC socket from the PMAC side.
 *  Puts the PMAC into a known communications state
 */
void lspmac_SockFlush() {
  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_FLUSH, 0, 0, 0, NULL, NULL, 1, NULL);
}


/** Clear the queue and put the PMAC into a known state.
 */
void lspmac_Reset() {
  ls_pmac_state = LS_PMAC_STATE_IDLE;

  // clear queue
  ethCmdReply = ethCmdOn;
  ethCmdOff   = ethCmdOn;

  lspmac_SockFlush();
}



/** The service routing detected an error condition.
 *  Scan the response buffer for an error code
 *  and print it out.
 */
void lspmac_Error(
		  char *buff	/**< [in] Buffer returned by PMAC perhaps containing a NULL terminated message.	*/
		  ) {
  int err;
  //
  // assume buff points to a 1400 byte array of stuff read from the pmac
  //

  if( buff[0] == 7 && buff[1] == 'E' && buff[2] == 'R' && buff[3] == 'R') {
    buff[7] = 0;  // For null termination
    err = atoi( &(buff[4]));
    if( err > 0 && err < 20) {
      lslogging_log_message( pmac_error_strs[err]);
    }
  }
  lspmac_Reset();
}


/** Service routine for packet coming from the PMAC.
 *  All communications is asynchronous so this is the only
 *  place incomming packets are handled
 */

void lspmac_Service(
		    struct pollfd *evt		/**< [in] pollfd object returned by poll		*/
		    ) {
  static char *receiveBuffer = NULL;	// the buffer inwhich to stick our incomming characters
  static int receiveBufferSize = 0;		// size of receiveBuffer
  static int receiveBufferIn = 0;		// next location to write to in receiveBuffer
  pmac_cmd_queue_t *cmd;			// maybe the command we are servicing
  ssize_t nsent, nread;				// nbytes dealt with
  int i;					// loop counter
  int foundEOCR;				// end of command response flag

  if( evt->revents & (POLLERR | POLLHUP | POLLNVAL)) {
    if( evt->fd != -1) {
      close( evt->fd);
      evt->fd = -1;
    }
    ls_pmac_state = LS_PMAC_STATE_DETACHED;
    return;
  }


  if( evt->revents & POLLOUT) {

    switch( ls_pmac_state) {
    case LS_PMAC_STATE_DETACHED:
      break;
    case LS_PMAC_STATE_IDLE:
      break;

    case LS_PMAC_STATE_SC:
      cmd = lspmac_pop_queue();
      if( cmd == NULL)
	return;

      if( cmd->pcmd.Request == VR_PMAC_GETMEM) {
	nsent = send( evt->fd, cmd, pmac_cmd_size, 0);
	if( nsent != pmac_cmd_size) {
	  lslogging_log_message( "Could only send %d of %d bytes....Not good.", (int)nsent, (int)(pmac_cmd_size));
	}
      } else {
	nsent = send( evt->fd, cmd, pmac_cmd_size + ntohs(cmd->pcmd.wLength), 0);
	gettimeofday( &pmac_time_sent, NULL);
	if( nsent != pmac_cmd_size + ntohs(cmd->pcmd.wLength)) {
	  lslogging_log_message( "Could only send %d of %d bytes....Not good.", (int)nsent, (int)(pmac_cmd_size + ntohs(cmd->pcmd.wLength)));
	}
      }

      if( cmd->pcmd.Request == VR_PMAC_SENDCTRLCHAR)
	ls_pmac_state = LS_PMAC_STATE_WACK_CC;
      else if( cmd->pcmd.Request == VR_CTRL_RESPONSE)
	ls_pmac_state = LS_PMAC_STATE_IDLE;
      else if( cmd->pcmd.Request == VR_PMAC_GETMEM)
	ls_pmac_state = LS_PMAC_STATE_GMR;
      else if( cmd->no_reply == 0)
	ls_pmac_state = LS_PMAC_STATE_WACK;
      else
	ls_pmac_state = LS_PMAC_STATE_WACK_NFR;
      break;

    case LS_PMAC_STATE_CR:
      /*
      switch( lspmac_control_char) {
      case 0x0002:	// Control-B    Report status word for 8 motors
      case 0x0003:	// Control-C    Report all coordinate system status words
      case 0x0006:	// Control-F    Report following errors for 8 motors
      case 0x0010:	// Control-P    Report positions for 8 motors
      case 0x0016:	// Control-V    Report velocity on 8 motors
      default:
	cr_cmd.wValue = htons(lspmac_control_char);
	cr_cmd.wLength = htons( 1400);
	nsent = send( evt->fd, &cr_cmd, pmac_cmd_size, 0);
	gettimeofday( &pmac_time_sent, NULL);
	ls_pmac_state = LS_PMAC_STATE_WCR;
	break;
      default:
	ls_pmac_state = LS_PMAC_STATE_IDLE;
      }
	*/
      break;

    case LS_PMAC_STATE_RR:
      nsent = send( evt->fd, &rr_cmd, pmac_cmd_size, 0);
      gettimeofday( &pmac_time_sent, NULL);
      ls_pmac_state = LS_PMAC_STATE_WACK_RR;
      break;

    case LS_PMAC_STATE_GB:
      nsent = send( evt->fd, &gb_cmd, pmac_cmd_size, 0);
      gettimeofday( &pmac_time_sent, NULL);
      ls_pmac_state = LS_PMAC_STATE_WGB;
      break;
    }
  }

  if( evt->revents & POLLIN) {

    if( receiveBufferSize - receiveBufferIn < 1400) {
      char *newbuff;

      receiveBufferSize += 1400;
      newbuff = calloc( receiveBufferSize, sizeof( unsigned char));
      if( newbuff == NULL) {
	lslogging_log_message( "lspmac_Service: Out of memory");
	exit( -1);
      }
      if( receiveBuffer != NULL) {
	memcpy( newbuff, receiveBuffer, receiveBufferIn);
	free(receiveBuffer);
      }
      receiveBuffer = newbuff;
    }

    nread = read( evt->fd, receiveBuffer + receiveBufferIn, 1400);

    foundEOCR = 0;
    if( ls_pmac_state == LS_PMAC_STATE_GMR) {
      //
      // get memory returns binary stuff, don't try to parse it
      //
      receiveBufferIn += nread;
    } else {
      //
      // other commands end in 6 if OK, 7 if not
      //
      for( i=receiveBufferIn; i<receiveBufferIn+nread; i++) {
	if( receiveBuffer[i] == 7) {
	  //
	  // Error condition
	  //
	  lspmac_Error( &(receiveBuffer[i]));
	  receiveBufferIn = 0;
	  return;
	}
	if( receiveBuffer[i] == 6) {
	  //
	  // End of command response
	  //
	  foundEOCR = 1;
	  receiveBuffer[i] = 0;
	  break;
	}
      }
      receiveBufferIn = i;
    }

    cmd = NULL;

    switch( ls_pmac_state) {
    case LS_PMAC_STATE_WACK_NFR:
      receiveBuffer[--receiveBufferIn] = 0;
      cmd = lspmac_pop_reply();
      ls_pmac_state = LS_PMAC_STATE_IDLE;
      break;
    case LS_PMAC_STATE_WACK:
      receiveBuffer[--receiveBufferIn] = 0;
      ls_pmac_state = LS_PMAC_STATE_RR;
      break;
    case LS_PMAC_STATE_WACK_CC:
      receiveBuffer[--receiveBufferIn] = 0;
      ls_pmac_state = LS_PMAC_STATE_CR;
      break;
    case LS_PMAC_STATE_WACK_RR:
      receiveBufferIn -= 2;
      if( receiveBuffer[receiveBufferIn])
	ls_pmac_state = LS_PMAC_STATE_GB;
      else
	ls_pmac_state = LS_PMAC_STATE_RR;
      receiveBuffer[receiveBufferIn] = 0;
      break;
    case LS_PMAC_STATE_GMR:
      cmd = lspmac_pop_reply();
      ls_pmac_state = LS_PMAC_STATE_IDLE;
      break;

    case LS_PMAC_STATE_WCR:
      cmd = lspmac_pop_reply();
      ls_pmac_state = LS_PMAC_STATE_IDLE;
      break;
    case LS_PMAC_STATE_WGB:
      if( foundEOCR) {
	cmd = lspmac_pop_reply();
	ls_pmac_state = LS_PMAC_STATE_IDLE;
      } else {
	ls_pmac_state = LS_PMAC_STATE_RR;
      }
      break;
    }


    if( cmd != NULL && cmd->onResponse != NULL) {
      cmd->onResponse( cmd, receiveBufferIn, receiveBuffer);
      receiveBufferIn = 0;
    }
  }
}



/** Receive a reply that does not require multiple buffers
 */
void lspmac_GetShortReplyCB(
			    pmac_cmd_queue_t *cmd,	/**< [in] Queue item this is a reply to		*/
			    int nreceived,		/**< [in] Number of bytes received		*/
			    char *buff			/**< [in] The buffer of bytes			*/
			    ) {

  char *sp;	// pointer to the command this is a reply to
  char *tmp;

  if( nreceived < 1400)
    buff[nreceived]=0;

  sp = (char *)(cmd->pcmd.bData);

  if( *buff == 0) {
    lslogging_log_message( "%s", sp);
  } else {
    tmp = cleanstr( buff);
    lslogging_log_message( "%s: %s", sp, tmp);
    free( tmp);
  }

  memset( cmd->pcmd.bData, 0, sizeof( cmd->pcmd.bData));
}

/** Receive a reply to a control character
 *  Print a "printable" version of the character to the terminal
 *  Followed by a hex dump of the response.
 */
void lspmac_SendControlReplyPrintCB(
				    pmac_cmd_queue_t *cmd,	/**< [in] Queue item this is a reply to		*/
				    int nreceived,		/**< [in] Number of bytes received		*/
				    char *buff			/**< [in] Buffer of bytes received		*/
				    ) {
  
  char *sp;
  int i;

  sp = calloc( nreceived+1, 1);
  for( i=0; i<nreceived; i++) {
    if( buff[i] == 0)
      break;
    if( isascii(buff[i]) && !iscntrl(buff[i]))
      sp[i] = buff[i];
    else
      sp[i] = ' ';
  }
  sp[i] = 0;

  lslogging_log_message( "control-%c: %s", '@'+ ntohs(cmd->pcmd.wValue), sp);
  free( sp);
}


/** Service a reply to the getmem command.
 * \param cmd Queue item this is a reply to
 * \param nreceived Number of bytes received
 * \param buff Buffer of bytes recieved
 *  
 */
void lspmac_GetmemReplyCB( pmac_cmd_queue_t *cmd, int nreceived, char *buff) {

  memcpy( &(dbmem[ntohs(cmd->pcmd.wValue)]), buff, nreceived);

  dbmemIn += nreceived;
  if( dbmemIn >= sizeof( dbmem)) {
    dbmemIn = 0;
  }
}

/** Request a chunk of memory to be returned.
 */
pmac_cmd_queue_t *lspmac_SockGetmem(
				    int offset,			/**< [in] Offset in PMAC Double Buffer		*/
				    int nbytes			/**< [in] Number of bytes to request		*/
				    )  {
  return lspmac_send_command( VR_UPLOAD,   VR_PMAC_GETMEM, offset, 0, nbytes, NULL, lspmac_GetmemReplyCB, 0, NULL);
}

/** Send a one line command.
 *  Uses printf style arguments.
 */
pmac_cmd_queue_t *lspmac_SockSendline( 
				      char *event,		/**< [in] base name for events				*/
				      char *fmt,		/**< [in] Printf style format string			*/
				      ...			/*        other arguments required by format string	*/
				       ) {
  va_list arg_ptr;
  char payload[1400];

  va_start( arg_ptr, fmt);
  vsnprintf( payload, sizeof(payload)-1, fmt, arg_ptr);
  payload[ sizeof(payload)-1] = 0;
  va_end( arg_ptr);

  lslogging_log_message( "%s", payload);

  return lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( payload), payload, lspmac_GetShortReplyCB, 0, event);
}




/** Send a command and ignore the response
 */
pmac_cmd_queue_t *lspmac_SockSendline_nr(
					 char *event,		/**< [in] base name for events				*/
					 char *fmt,		/**< [in] Printf style format string			*/
					 ...			/*        Other arguments required by format string	*/
					 ) {
  va_list arg_ptr;
  char s[512];

  va_start( arg_ptr, fmt);
  vsnprintf( s, sizeof(s)-1, fmt, arg_ptr);
  s[sizeof(s)-1] = 0;
  va_end( arg_ptr);

  lslogging_log_message( s);

  return lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( s), s, NULL, 1, event);
}

/** Send a control character
 */
pmac_cmd_queue_t *lspmac_SockSendControlCharPrint(
						  char *event,	/**< [in] base name for events				*/
						  char c	/**< The control character to send			*/
						  ) {
  lspmac_control_char = c;
  //  return lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDCTRLCHAR, c, 0, 1400, NULL, lspmac_SendControlReplyPrintCB, 0, event);
  return lspmac_send_command( VR_UPLOAD, VR_CTRL_RESPONSE, c, 0, 1400, NULL, lspmac_SendControlReplyPrintCB, 0, event);
}

/** Request a block of double buffer memory.
 */
void lspmac_Getmem() {
  int nbytes;
  nbytes = (dbmemIn + 1400 > sizeof( dbmem)) ? sizeof( dbmem) - dbmemIn : 1400;
  lspmac_SockGetmem( dbmemIn, nbytes);
}

/** Read the state of a binary i/o motor
 *  This is the read method for the binary i/o motor class
 */
void lspmac_bo_read(
		    lspmac_motor_t *mp		/**< [in] The motor			*/
		    ) {
  int pos, changed;
  char *fmt;

  pthread_mutex_lock( &(mp->mutex));

  pos = (*(mp->read_ptr) & mp->read_mask) == 0 ? 0 : 1;

  changed = pos != mp->position;
  mp->position = pos;

  if( changed) {
    mp->motion_seen  = 1;
    mp->not_done     = 0;
    mp->command_sent = 1;
    pthread_cond_signal( &(mp->cond));
    lsevents_send_event( "%s Moving", mp->name);
    lsevents_send_event( "%s %d", mp->name, pos);
    lsevents_send_event( "%s In Position", mp->name);
  }

  if( mp->reported_position != mp->position) {
    fmt = lsredis_getstr(mp->redis_fmt);
    lsredis_setstr( mp->redis_position, fmt, mp->position);
    free(fmt);
    mp->reported_position = mp->position;
  }

  pthread_mutex_unlock( &(mp->mutex));
}

/** Read a DAC motor position
 */
void lspmac_dac_read(
		     lspmac_motor_t *mp		/**< [in] The motor			*/
		     ) {
  double u2c;
  char *fmt;

  pthread_mutex_lock( &(mp->mutex));
  mp->actual_pos_cnts = *mp->actual_pos_cnts_p;
  u2c = lsredis_getd( mp->u2c);

  if( mp->nlut >0 && mp->lut != NULL) {
    if( u2c == 0.0)
      u2c = 1.0;
    mp->position = lspmac_rlut( mp->nlut, mp->lut, mp->actual_pos_cnts/u2c);
  } else {
    if( u2c != 0.0) {
      mp->position = mp->actual_pos_cnts / u2c;
    } else {
      mp->position = mp->actual_pos_cnts;
    }
  }

  if( fabs(mp->reported_position - mp->position) >= lsredis_getd(mp->update_resolution)) {
    fmt = lsredis_getstr(mp->redis_fmt);
    lsredis_setstr( mp->redis_position, fmt, mp->position);
    free(fmt);
    mp->reported_position = mp->position;
  }

  pthread_mutex_unlock( &(mp->mutex));
}

/** Fast shutter read routine
 *  The shutter is mildly complicated in that we need to take into account the fact that
 *  the shutter can open and close again between status updates.  This means that we need
 *  to rely on a PCL program running in the PMAC to monitor the shutter state and let us
 *  know that this has happened.
 */
void lspmac_shutter_read(
			 lspmac_motor_t *mp	/**< [in] The motor object associated with the fast shutter	*/
			 ) {
  char *fmt;
  //
  // track the shutter state and signal if it has changed
  //
  pthread_mutex_lock( &lspmac_shutter_mutex);
  if( md2_status.fs_has_opened && !lspmac_shutter_has_opened && !md2_status.fs_is_open) {
    //
    // Here the shutter opened and closed again before we got the memo
    // Treat it as a shutter closed event
    //
    pthread_cond_signal( &lspmac_shutter_cond);
  }
  lspmac_shutter_has_opened = md2_status.fs_has_opened;

  if( lspmac_shutter_state !=  md2_status.fs_is_open) {
    lspmac_shutter_state = md2_status.fs_is_open;
    pthread_cond_signal( &lspmac_shutter_cond);
  }

  pthread_mutex_lock( &ncurses_mutex);
  if( md2_status.fs_is_open) {
    mvwprintw( term_status2, 1, 1, "Shutter Open  ");
    mp->position = 1;
  } else {
    mvwprintw( term_status2, 1, 1, "Shutter Closed");
    mp->position = 0;
  }
  pthread_mutex_unlock( &ncurses_mutex);

  if( fshut->reported_position != fshut->position) {
    fmt = lsredis_getstr( fshut->redis_fmt);
    lsredis_setstr( fshut->redis_position, fmt, fshut->position);
    free(fmt);
    fshut->reported_position = fshut->position;
  }

  pthread_mutex_unlock( &lspmac_shutter_mutex);
}

/** Home the motor.
 */
void lspmac_home1_queue(
			lspmac_motor_t *mp			/**< [in] motor we are concerned about		*/
			) {
  int i;
  int motor_num;
  int coord_num;
  char **home;

  pthread_mutex_lock( &(mp->mutex));

  motor_num = lsredis_getl( mp->motor_num);
  coord_num = lsredis_getl( mp->coord_num);
  home      = lsredis_get_string_array( mp->home);
  
  // Each of the motors should have this defined
  // but let's not seg fault if home is missing
  //
  if( home == NULL || *home == NULL) {
    //
    // Note we are already initialized
    // so if we are here there is something wrong.
    //
    lslogging_log_message( "lspmac_home1_queue: null or empty home strings for motor %s", mp->name);
    pthread_mutex_unlock( &(mp->mutex));
    return;
  }

  // We've already been called.  Don't home again until
  // we're finish with the last time.
  //
  if( mp->homing) {
    pthread_mutex_unlock( &(mp->mutex));
    return;
  }    


  //
  // Don't go on if any other motors in this coordinate system are homing.
  // It's possible to write the homing program to home all the motors in the coordinate
  // system.  TODO  (hint hint)
  //
  if( coord_num > 0) {
    for( i=0; i<lspmac_nmotors; i++) {
      if( &(lspmac_motors[i]) == mp)
	continue;
      if( lsredis_getl(lspmac_motors[i].coord_num) == coord_num) {
	int nogo;
	nogo = 0;
	pthread_mutex_lock( &(lspmac_motors[i].mutex));
	//
	//  Don't go on if
	//
	//    we are homing         or      ( not in position                while     in open loop)
	//
	if( lspmac_motors[i].homing || (((lspmac_motors[i].status2 & 0x01)==0) && ((lspmac_motors[i].status1 & 0x040000) != 0)))
	  nogo = 1;
	pthread_mutex_unlock( &(lspmac_motors[i].mutex));
	if( nogo) {
	  pthread_mutex_unlock( &(mp->mutex));
	  return;
	}
      }
    }
  }
  mp->homing   = 1;
  mp->not_done = 1;	// set up waiting for cond
  mp->motion_seen = 0;
  // This opens the control loop.
  // The status routine should notice this and the fact that
  // the homing flag is set and call on the home2 routine
  //
  // Only send the open loop command if we are not in
  // open loop mode already.  This test might prevent a race condition
  // where we've already moved the home2 routine (and queue the homing program motion)
  // before the open loop command is dequeued and acted on.
  //
  if( ~(mp->status1) & 0x040000) {
    lspmac_SockSendDPline( mp->name, "#%d$*", motor_num);
  }

  pthread_mutex_unlock( &(mp->mutex));

  lsevents_send_event( "%s Homing", mp->name);
}

/** Second stage of homing
 */

void lspmac_home2_queue(
			lspmac_motor_t *mp			/**< [in] motor we are concerned about		*/
			) {

  char **spp;
  char **home;

  //
  // At this point we are in open loop.
  // Run the motor specific commands
  //

  pthread_mutex_lock( &(mp->mutex));

  home = lsredis_get_string_array( mp->home);

  //
  // We don't have any motors that have a null home text array so 
  // there is currently no need to worry about this case other than
  // not to seg fault
  //
  // Also, Only go on if the first homing phase has been started
  //
  if( home == NULL || mp->homing != 1) {
    pthread_mutex_unlock( &(mp->mutex));
    return;
  }

  for( spp = home; *spp != NULL; spp++) {

    lslogging_log_message( "home2 is queuing '%s'\n", *spp);

    lspmac_SockSendDPline( mp->name, *spp);
  }

  mp->homing = 2;
  pthread_mutex_unlock( &(mp->mutex));
}

/** get the motor position (with locking)
 *  \param mp the motor object
 */
double lspmac_getPosition( lspmac_motor_t *mp) {
  double rtn;
  pthread_mutex_lock( &(mp->mutex));
  rtn = mp->position;
  pthread_mutex_unlock( &(mp->mutex));
  return rtn;
}


/** Read the position and status of a normal PMAC motor
 */
void lspmac_pmacmotor_read(
			   lspmac_motor_t *mp		/**< [in] Our motor		*/
			   ) {
  char s[512], *sp;
  int homing1, homing2;
  double u2c;
  double neutral_pos;
  int motor_num;
  char *fmt;
  int status_changed;


  if( lsredis_getb( mp->active) != 1)
    return;

  pthread_mutex_lock( &(mp->mutex));

  //
  // if this time and last time were both "in position"
  // and the position changed significantly then log the event
  //
  // On E omega has been observed to change by 0x10000 on its own
  // with no real motion.
  //
  if( mp->status2 & 1 && mp->status2 == *mp->status2_p && abs( mp->actual_pos_cnts - *mp->actual_pos_cnts_p) > 256) {
    //    lslogging_log_message( "Instantaneous change: %s old status1: %0x, new status1: %0x, old status2: %0x, new status2: %0x, old cnts: %0x, new cnts: %0x",
    //			   mp->name, mp->status1, *mp->status1_p, mp->status2, *mp->status2_p, mp->actual_pos_cnts, *mp->actual_pos_cnts_p);

    //
    // At this point we'll just log the event and return
    // There is no reason to believe the change is real.
    //
    // There is a non-zero probability that the first value is the bad one and any value afterwards will be taken as
    // wrong.  Homing (or moving) the motor should fix this.  There is a non-zero probably that it can happen
    // two or more times in a row after moving.
    //
    // TODO: account for the case where mp->actual_pos_cnts is the bad value.
    //
    // TODO: Is this a problem when the motor is moving?  Can we detect it?
    //
    // TODO: Think of the correct change value here (currently 256) that works for all motors
    // or have this value configurable
    //
    pthread_mutex_unlock( &(mp->mutex));
    return;
  }


  // Send an event if inPosition has changed
  //
  if( (mp->status2 & 0x000001) != (*mp->status2_p & 0x000001)) {
    lsevents_send_event( "%s %s", mp->name, (*mp->status2_p & 0x000001) ? "In Position" : "Moving");
  }

  // Get some values we might need later
  //
  u2c         = lsredis_getd( mp->u2c);
  motor_num   = lsredis_getl( mp->motor_num);
  neutral_pos = lsredis_getd( mp->neutral_pos);

  //
  // maybe look for omega zero crossing
  // 
  if( motor_num == 1 && omega_zero_search && *mp->actual_pos_cnts_p >=0 && mp->actual_pos_cnts < 0) {
    int secs, nsecs;

    if( omega_zero_velocity > 0.0) {
      secs = *mp->actual_pos_cnts_p / omega_zero_velocity;
      nsecs = (*mp->actual_pos_cnts_p / omega_zero_velocity - secs) * 1000000000;


      omega_zero_time.tv_sec = lspmac_status_time.tv_sec  - secs;
      omega_zero_time.tv_nsec= lspmac_status_time.tv_nsec;
      if( omega_zero_time.tv_nsec < nsecs) {
	omega_zero_time.tv_sec  -= 1;
	omega_zero_time.tv_nsec += 1000000000;
      }
      omega_zero_time.tv_nsec -= nsecs;

      lsevents_send_event( "omega crossed zero");
      lslogging_log_message("lspmac_pmacmotor_read: omega zero secs %d  nsecs %d ozt.tv_sec %ld  ozt.tv_nsec  %ld, motor cnts %d",
			    secs, nsecs, omega_zero_time.tv_sec, omega_zero_time.tv_nsec, *mp->actual_pos_cnts_p);
    }
    omega_zero_search = 0;
  }


  // Make local copies so we can inspect them in other threads
  // without having to grab the status mutex
  //
  if( mp->status1 != *mp->status1_p || mp->status2 != *mp->status2_p) {
    mp->status1 = *mp->status1_p;
    mp->status2 = *mp->status2_p;
    status_changed = 1;
  } else {
    status_changed = 0;
  }
  mp->actual_pos_cnts = *mp->actual_pos_cnts_p;

  //
  // See if we are done moving, ie, in position
  //
  if( mp->status2 & 0x000001) {
    if( mp->not_done) {
      mp->not_done = 0;
      pthread_cond_signal( &(mp->cond));
    }
  } else if( mp->not_done == 0) {
    mp->not_done = 1;
  }

  // See if the motor is moving
  //
  //                move timer                  homing
  //                  123456                    123456
  if( mp->status1 & 0x020000 || mp->status1 & 0x000400) {
    if( mp->motion_seen == 0) {
      mp->motion_seen = 1;
      pthread_cond_signal( &(mp->cond));
    }
  }

  pthread_mutex_lock( &ncurses_mutex);
  mvwprintw( mp->win, 2, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, " ");
  mvwprintw( mp->win, 2, 1, "%*d cts", LS_DISPLAY_WINDOW_WIDTH-6, mp->actual_pos_cnts);
  mvwprintw( mp->win, 3, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, " ");
  pthread_mutex_unlock( &ncurses_mutex);

  if( mp->nlut >0 && mp->lut != NULL) {
    mp->position = lspmac_rlut( mp->nlut, mp->lut, mp->actual_pos_cnts);
  } else {
    if( u2c != 0.0) {
      mp->position = ((mp->actual_pos_cnts / u2c) - neutral_pos);
    } else {
      mp->position = mp->actual_pos_cnts;
    }
  }

  if( status_changed || fabs(mp->reported_position - mp->position) >= lsredis_getd(mp->update_resolution)) {
    fmt = lsredis_getstr(mp->redis_fmt);
    lsredis_setstr( mp->redis_position, fmt, mp->position);
    free(fmt);
    mp->reported_position = mp->position;
  }

  fmt = lsredis_getstr( mp->printf_fmt);
  snprintf( s, sizeof(s)-1, fmt, 8, mp->position);
  s[sizeof(s)-1] = 0;
  free( fmt);

  //
  // indicate limit problems
  //
  lsredis_setstr( mp->pos_limit_hit, mp->status1 & 0x200000 ? "1" : "0");
  lsredis_setstr( mp->neg_limit_hit, mp->status1 & 0x400000 ? "1" : "0");



  // set flag if we are not homed
  homing1 = 0;
  //                        ~(homed flag)
  if( mp->homing == 0  && (~mp->status2 & 0x000400) != 0) {
    homing1 = 1;
  }

  // set flag if we are homing and in open loop
  homing2 = 0;
  //                         open loop
  if( mp->homing == 1 && (mp->status1 & 0x040000) != 0) {
    homing2 = 1;
  }
  // maybe reset homing flag
  //                        homed flag                       in position flag
  if( (mp->homing == 2) && ((mp->status2 & 0x000400) != 0) && ((mp->status2 & 0x000001) != 0)) {
    mp->homing = 0;
    lsevents_send_event( "%s Homed", mp->name);
  }

  pthread_mutex_lock( &ncurses_mutex);
  s[sizeof(s)-1] = 0;
  mvwprintw( mp->win, 3, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-6, s);

  if( status_changed) {
    mvwprintw( mp->win, 4, 1, "%*x", LS_DISPLAY_WINDOW_WIDTH-2, mp->status1);
    mvwprintw( mp->win, 5, 1, "%*x", LS_DISPLAY_WINDOW_WIDTH-2, mp->status2);
    sp = "";
    if( mp->status2 & 0x000002)
      sp = "Following Warning";
    else if( mp->status2 & 0x000004)
      sp = "Following Error";
    else if( mp->status2 & 0x000020)
      sp = "I2T Amp Fault";
    else if( mp->status2 & 0x000008)
      sp = "Amp. Fault";
    else if( mp->status2 & 0x000800)
      sp = "Stopped on Limit";
    else if( mp->status1 & 0x040000)
      sp = "Open Loop";
    else if( ~(mp->status1) & 0x080000)
      sp = "Motor Disabled";
    else if( mp->status1 & 0x000400)
      sp = "Homing";
    else if( (mp->status1 & 0x600000) == 0x600000)
      sp = "Both Limits Tripped";
    else if( mp->status1 & 0x200000)
      sp = "Positive Limit";
    else if( mp->status1 & 0x400000)
      sp = "Negative Limit";
    else if( ~(mp->status2) & 0x000400)
      sp = "Not Homed";
    else if( mp->status1 & 0x020000)
      sp = "Moving";
    else if( mp->status2 & 0x000001)
      sp = "In Position";

    mvwprintw( mp->win, 6, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, sp);
  
    lsredis_setstr( mp->status_str, sp);
  }
  wnoutrefresh( mp->win);
  pthread_mutex_unlock( &ncurses_mutex);

  pthread_mutex_unlock( &(mp->mutex));

  if( homing1)
    lspmac_home1_queue( mp);

  if( homing2)
    lspmac_home2_queue( mp);

  lspmac_status_last_time.tv_sec  = lspmac_status_time.tv_sec;
  lspmac_status_last_time.tv_nsec = lspmac_status_time.tv_nsec;
}

/** get binary input value
 */
int lspmac_getBIPosition( lspmac_bi_t *bip) {
  int rtn;
  pthread_mutex_lock( &bip->mutex);
  rtn = bip->position;
  pthread_mutex_unlock( &bip->mutex);
  return rtn;
}


/** Service routing for status upate
 *  This updates positions and status information.
 */
void lspmac_get_status_cb(
			  pmac_cmd_queue_t *cmd,		/**< [in] The command that generated this reply	*/
			  int nreceived,			/**< [in] Number of bytes received		*/
			  char *buff				/**< [in] The Big Byte Buffer			*/
			  ) {
  #ifdef SHOW_RATE
  static struct timespec ts1;
  static struct timespec ts2;
  static int cnt = 0;
  #endif

  int i;
  lspmac_bi_t    *bp;

  clock_gettime( CLOCK_REALTIME, &lspmac_status_time);

  #ifdef SHOW_RATE
  if( cnt == 0) {
    clock_gettime( CLOCK_REALTIME, &ts1);
  }
  #endif

  pthread_mutex_lock( &md2_status_mutex);
  memcpy( &md2_status, buff, sizeof(md2_status));
  //
  // Note that we are the only thread that writes to md2_status
  // so we no longer need the lock to read.  Other threads must
  // lock the mutex to read md2_status.
  //
  pthread_mutex_unlock( &md2_status_mutex);


  //
  // track the coordinate system moving flags
  //
  pthread_mutex_lock( &lspmac_moving_mutex);
  if( md2_status.moving_flags != lspmac_moving_flags) {
    int mask;

    lslogging_log_message( "lspmac_get_status_cb: new moving flag: %0x", md2_status.moving_flags);
    mask = 1;
    for( i=1; i<=16; i++, mask <<= 1) {
      if( ((lspmac_moving_flags & mask) != 0) && ((md2_status.moving_flags & mask) == 0)) {
	// Falling edge: send event
	lsevents_send_event( "Coordsys %d Stopped", i);
      }
    }
    lspmac_moving_flags = md2_status.moving_flags;
    pthread_cond_signal( &lspmac_moving_cond);
  }
  pthread_mutex_unlock( &lspmac_moving_mutex);


  //
  // Read the motor positions
  //
  for( i=0; i<lspmac_nmotors; i++) {
    lspmac_motors[i].read(&(lspmac_motors[i]));
  }

  //
  // Read the binary inputs and perhaps send an event
  //
  for( i=0; i<lspmac_nbis; i++) {
    bp = &(lspmac_bis[i]);
    
    pthread_mutex_lock( &(bp->mutex));

    bp->position = (*(bp->ptr) & bp->mask) == 0 ? 0 : 1;

    if( bp->first_time) {
      bp->first_time = 0;
      if( bp->position==1 && bp->changeEventOn != NULL && bp->changeEventOn[0] != 0)
	lsevents_send_event( lspmac_bis[i].changeEventOn);
      if( bp->position==0 && bp->changeEventOff != NULL && bp->changeEventOff[0] != 0)
	lsevents_send_event( lspmac_bis[i].changeEventOff);
    } else {
      if( bp->position != bp->previous) {
	if( bp->position==1 && bp->changeEventOn != NULL && bp->changeEventOn[0] != 0)
	  lsevents_send_event( lspmac_bis[i].changeEventOn);
	if(bp->position==0 && bp->changeEventOff != NULL && bp->changeEventOff[0] != 0)
	  lsevents_send_event( lspmac_bis[i].changeEventOff);
      }
    }
    bp->previous = bp->position;
    pthread_mutex_unlock( &(bp->mutex));
  }

  pthread_mutex_lock( &ncurses_mutex);

  // acc11c_1	INPUTS
  // mask  bit
  // 0x01  0	M1000	Air pressure OK
  // 0x02  1	M1001	Air bearing OK
  // 0x04  2	M1002	Cryo switch
  // 0x08  3    M1003	Backlight Down
  // 0x10  4    M1004	Backlight Up
  // 0x20  5
  // 0x40  6	M1006	Cryo is back

  //
  // acc11c_2	INPUTS
  // mask  bit
  // 0x01  0	M1008	Fluor Dector back
  // 0x02  1	M1009	Sample Detected
  // 0x04  2	M1020	{SC load request}
  // 0x08  3	M1021	{SC move cryo back request}
  // 0x10  4	M1022	{SC sample magnet control}
  // 0x20  5	M1013	Etel Ready
  // 0x40  6	M1014	Etel On
  // 0x80  7	M1015	Etel Init OK

  if( md2_status.acc11c_2 & 0x01)
    mvwprintw( term_status2, 3, 10, "%*s", -8, "Fluor Out");
  else
    mvwprintw( term_status2, 3, 10, "%*s", -8, "Fluor In");

  if( md2_status.acc11c_5 & 0x08)
    mvwprintw( term_status2, 4, 1, "%*s", -(LS_DISPLAY_WINDOW_WIDTH-2), "Dryer On");
  else
    mvwprintw( term_status2, 4, 1, "%*s", -(LS_DISPLAY_WINDOW_WIDTH-2), "Dryer Off");

  if( md2_status.acc11c_2 & 0x02)
    mvwprintw( term_status2, 2, 1, "%*s", -(LS_DISPLAY_WINDOW_WIDTH-2), "Cap Dectected");
  else
    mvwprintw( term_status2, 2, 1, "%*s", -(LS_DISPLAY_WINDOW_WIDTH-2), "Cap Not Dectected");
  wnoutrefresh( term_status2);


  // acc11c_3	INPUTS
  // mask  bit
  // 0x01  0	M1025	Minikappa OK
  // 0x02  1    M1023	{SC unload request}
  // 0x04  2    M1024	Smartmagnet is on (note in pmc saying this is not used in VB interface)
  // 0x08  3	M1027	Arm Parked
  // 0x10  4    M1031	Smartmagnet error (coil is broken)
  // 0x20  5
  // 0x40  6
  // 0x80  7
  // 0x100 8    M1048	Shutter is open (note in pmc says: slow input !!!)

  // acc11c_4	INPUTS
  // mask  bit
  // 0x01  0    M1031	{laser mirror is back}
  // 0x02  1    M1032	{laser PSS OK}
  // 0x04  2    M1033	{laser shutter open}

  


  // acc11c_5	OUTPUTS
  // mask  bit
  // 0x01  0    M1100	Mag Off
  // 0x02  1    M1191	Condenser Out
  // 0x04  2    M1102	Cryo Back
  // 0x08  3    M1103	Dryer On
  // 0x10  4    M1104	FluoDet Out
  // 0x20  5    M1105	{smartmagnet on/off: note in pmc says this is not used}
  // 0x40  6    M1106	1=SmartMag, 0=Permanent Mag
  //

  if( md2_status.acc11c_5 & 0x04)
    mvwprintw( term_status2, 3, 1, "%*s", -8, "Cryo Out");
  else
    mvwprintw( term_status2, 3, 1, "%*s", -8, "Cryo In ");

  // acc11c_6	OUTPUTS
  // mask   bit
  // 0x0001   0	M1040   {SC Sample transfer is on}
  // 0x0002   1
  // 0x0004   2
  // 0x0008   3
  // 0x0010   4
  // 0x0020   5
  // 0x0040   6
  // 0x0080   7	M1115   Etel Enable
  // 0x0100   8 M1124   Fast Shutter Enable
  // 0x0200   9 M1125   Fast Shutter Manual Enable
  // 0x0400  10 M1126   Fast Shutter On
  // 0x0800  11  
  // 0x1000  12 M1128   ADC1 gain bit 0
  // 0x2000  13 M1129   ADC1 gain bit 1
  // 0x4000  14 M1130   ADC2 gain bit 0
  // 0x8000  15 M1131   ADC2 gain bit 1
  //

  if( md2_status.acc11c_5 & 0x02)
    mvwprintw( term_status,  3, 1, "%*s", -(LS_DISPLAY_WINDOW_WIDTH-2), "Backlight Up");
  else
    mvwprintw( term_status,  3, 1, "%*s", -(LS_DISPLAY_WINDOW_WIDTH-2), "Backlight Down");
  mvwprintw( term_status, 4, 1, "Front: %*u", LS_DISPLAY_WINDOW_WIDTH-2-8, (int)flight->position);
  mvwprintw( term_status, 5, 1, "Back: %*u", LS_DISPLAY_WINDOW_WIDTH-2-7,  (int)blight->position);
  mvwprintw( term_status, 6, 1, "Piezo: %*u", LS_DISPLAY_WINDOW_WIDTH-2-8, (int)fscint->position);
  wnoutrefresh( term_status);

  wnoutrefresh( term_input);
  doupdate();
  pthread_mutex_unlock( &ncurses_mutex);

  #ifdef SHOW_RATE
  if( ++cnt % 1000 == 0) {
    long diff_sec;
    long diff_nsec;

    clock_gettime( CLOCK_REALTIME, &ts2);

    diff_sec  = ts2.tv_sec  - ts1.tv_sec;
    diff_nsec = ts2.tv_nsec - ts1.tv_nsec;

    if( diff_nsec < 0) {
      diff_nsec += 1000000000;
      diff_sec--;
    }

    lslogging_log_message( "Refresh Rate: %0.1f Hz", (double)cnt / (diff_sec + diff_nsec/1000000000.));

    cnt = 0;
  }
  #endif
}

/** Request a status update from the PMAC
 */
void lspmac_get_status() {
  lspmac_send_command( VR_UPLOAD, VR_PMAC_GETMEM, 0x400, 0, sizeof(md2_status_t), NULL, lspmac_get_status_cb, 0, NULL);
}

/** we are expecting more characters from the DPRAM ASCII interface
 */
void lspmac_more_ascii_cb( pmac_cmd_queue_t *cmd, int nreceived, char *buff) {
  lspmac_get_ascii( cmd->event);
}

/** service the ascii buffer request response
 */
void lspmac_get_ascii_cb( pmac_cmd_queue_t *cmd, int nreceived, char *buff) {
  uint32_t clrdata;
  int need_more;

  need_more = 0;
  pthread_mutex_lock( &lspmac_ascii_mutex);
  memcpy( &lspmac_ascii_buffers, buff, sizeof(lspmac_ascii_buffers));


  //
  // The response is not ready yet
  // This will be an infinite loop if we queue a command that does not
  // produce a response.
  //
  // Quoted comments below from Delta Tau "Turbo PMAC User Manual 9/12/2008, page 422"
  //
  // "1.  Wait for the Host-Input Control Word at 0x0F40 (Y:$063D0) to become greater than 0, indicating
  // that a response line is ready."
  //
  if( lspmac_ascii_buffers.response_buf == 0) {
    need_more = 1;
  } else {
    if( (lspmac_ascii_buffers.response_buf & 0x8000) != 0) {
      char bcd1, bcd2, bcd3;
      int errcode;
      // Error response
      //
      // "2.  Interpret the value in this register to determine what
      // type of response is present. If Bit 15 is 1, Turbo PMAC is
      // reporting an error in the command, and there is no response
      // other than this word. In this case, Bits 0 – 11 encode the
      // error number for the command as 3 BCD digits."
      //
      need_more = 0;
      bcd1 = lspmac_ascii_buffers.response_buf  & 0x000f;
      bcd2 = (lspmac_ascii_buffers.response_buf & 0x00f0) >> 4;
      bcd3 = (lspmac_ascii_buffers.response_buf & 0x0f00) >> 8;
      errcode = (bcd3 * 10 + bcd2) * 10 + bcd1;
      
      if( errcode >= sizeof( pmac_error_strs)/sizeof( *pmac_error_strs))
	errcode = 0;
      lslogging_log_message( "lspmac_get_ascii_cb: Error returned for %s: %s", lspmac_ascii_buffers.command_str, pmac_error_strs[errcode]);
      //
      // Command not allowed during program execution.
      //
      // Requeue it;
      if( errcode == 1) {
	lspmac_dpascii_off--;
      }
    } else {
      //
      // "3.  Read the response string starting at 0x0F44
      // (Y:$0603D1). Two 8-bit characters are packed into each 16-bit
      // word; the first character is placed into the low
      // byte. Subsequent characters are placed into consecutive
      // higher addresses, two per 16-bit word. (In byte addressing,
      // each character is read from an address one higher than the
      // preceding character.) Up to 255 characters can be sent in a
      // single response line. The string is terminated with the NULL
      // character (byte value 0), convenient for C-style string
      // handling. For Pascal-style string handling, the register at
      // 0x0F42 (X:$0603D0) contains the number of characters in the
      // string (plus one)."
      //
      
      if( cmd->event != NULL && strncmp( cmd->event, "Control-", 8) == 0) {
	lslogging_log_message( "%s: %s", cmd->event, lspmac_ascii_buffers.response_str);
	need_more = 0;
      } else {	
	if( lspmac_ascii_buffers.response_n > 1)
	  lslogging_log_message( "lspmac_get_ascii_cb: '%s'   '%s'", lspmac_ascii_buffers.command_str, lspmac_ascii_buffers.response_str);
	else
	  lslogging_log_message( "lspmac_get_ascii_cb: '%s'   responded", lspmac_ascii_buffers.command_str);

	//
	// 5.  "If Bits 0 – 7 of the Host-Input Control Word had
	// contained the value $0D (13 decimal, “CR”), this was not the
	// last line in the response, and steps 1 – 4 should be
	// repeated. If they had contained the value $06 (6 decimal,
	// “ACK”), this was the last line in the response."
	//
	if( (lspmac_ascii_buffers.response_buf & 0x00ff) == 0x0d) {
	  need_more = 1;
	} else {
	  need_more = 0;
	
	  if( cmd->event != NULL && *(cmd->event) != 0)
	    lsevents_send_event( "%s command accepted", cmd->event);
	}
      }
    } 
  }

  pthread_mutex_unlock( &lspmac_ascii_mutex);

  //
  // Reset the buffer flags and, perhaps, requeue a request
  //
  // "4.  Clear the Host-Input Control Word at 0x0F40 (Y:$063D0)
  // to 0. Turbo PMAC will not send another response line until it sees
  // this register set to 0."
  //
  clrdata = 0;		// set the control word to zero

  if( need_more) {
    lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0f40, 0, 4, (char *)&clrdata, lspmac_more_ascii_cb, 1, NULL);
  } else {
    lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0f40, 0, 4, (char *)&clrdata, NULL, 1, NULL);
    lspmac_ascii_busy = 0;
  }
}

/** Request the ascii buffers from the PMAC
 */
void lspmac_get_ascii( char *event) {
  lspmac_send_command( VR_UPLOAD, VR_PMAC_GETMEM, 0x0e9c, 0, sizeof(lspmac_ascii_buffers_t), NULL, lspmac_get_ascii_cb, 0, event);
}


/** PMAC has received our ascii command request
 *  Now see when it is ready for the next one
 */
void lspmac_asciicmdCB( pmac_cmd_queue_t *cmd, int nreceived, char *buf) {
  lspmac_get_ascii( cmd->event);
}

/** prepare (queue up) a line to send the dpram ascii command interface
 */
void lspmac_SockSendDPline( char *event, char *fmt, ...) {
  va_list arg_ptr;
  uint32_t index;
  char *pl;
  
  pthread_mutex_lock( &lspmac_ascii_mutex);
  index = lspmac_dpascii_on++ % LSPMAC_DPASCII_QUEUE_LENGTH;

  pl = lspmac_dpascii_queue[index].pl;

  va_start( arg_ptr, fmt);
  vsnprintf( pl, 159, fmt, arg_ptr);
  pl[159] = 0;
  va_end( arg_ptr);

  lspmac_dpascii_queue[index].event = event;

  pthread_mutex_unlock( &lspmac_ascii_mutex);
}

void lspmac_request_control_response_cb( char *event) {
  static char s[32];
  int i;

  for( i=0; i<31 && event[i] != 0; i++) {
    s[i] = 0;
    if( event[i] == ' ')
      break;
    s[i] = event[i];
  }
  s[i] = 0;
  lspmac_get_ascii( s);

}


void lspmac_SockSendDPControlCharCB( pmac_cmd_queue_t *cmd, int nreceived, char *buf) {
  if( cmd->event != NULL && *(cmd->event))
    lsevents_send_event( "%s accepted", cmd->event);
}

/** use dpram ascii interface to send a control character
 */
void lspmac_SockSendDPControlChar( char *event, char c) {
  uint16_t buff;

  buff = 0x07 & c;
  lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0e9e, 0, 2, (char *)&buff, lspmac_SockSendDPControlCharCB, 1, event);
  if( event != NULL)
    lsevents_send_event( "%s queued", event);
}


void lspmac_SockSendDPqueue() {
  lspmac_dpascii_queue_t *qp;
  uint32_t mask;
  uint32_t clrdata;

  pthread_mutex_lock( &lspmac_ascii_mutex);
  qp = &(lspmac_dpascii_queue[(lspmac_dpascii_off++) % LSPMAC_DPASCII_QUEUE_LENGTH]);
  lspmac_ascii_busy = 1;
  pthread_mutex_unlock( &lspmac_ascii_mutex);

  lslogging_log_message( "lspmac_SockSendDPqueue: %s", qp->pl);

  clrdata = 0;		// set the control word to zero
  lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0f40, 0, 4, (char *)&clrdata, NULL, 1, NULL);
  lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0e9c, 0, 4, (char *)&clrdata, NULL, 1, NULL);

  lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0ea0, 0, strlen(qp->pl)+1, qp->pl, NULL, 1, NULL);

  mask = 0x0001;
  lspmac_send_command( VR_UPLOAD, VR_PMAC_SETBIT, 0x0e9c, 1, sizeof( mask), (char *)&mask,lspmac_asciicmdCB, 1, qp->event);

  if( qp->event != NULL && *(qp->event) != 0)
    lsevents_send_event( "%s queued", qp->event);
}

/** abort motion and try to recover
 */
void lspmac_abort() {
  //
  // Stop everything!  (consider ^O instead of ^A)
  //
  lspmac_SockSendDPControlChar( "Abort Request", 0x01);

  //
  // and reset motion flag
  //
  lspmac_SockSendDPline( "Reset", "%s", "M5075=0");

}



/** Receive the values of all the I variables
 *  Update our Postgresql database with the results
 */
void lspmac_GetAllIVarsCB(
			  pmac_cmd_queue_t *cmd,	/**< [in] The command that gave this response	*/
			  int nreceived,		/**< [in] Number of bytes received		*/
			  char *buff			/**< [in] The byte buffer			*/
			  ) {
  static char qs[LS_PG_QUERY_STRING_LENGTH];
  char *sp;
  int i;
  for( i=0, sp=strtok(buff, "\r"); sp != NULL; sp=strtok( NULL, "\r"), i++) {
    snprintf( qs, sizeof( qs)-1, "SELECT pmac.md2_ivar_set( %d, '%s')", i, sp);
    qs[sizeof( qs)-1]=0;
    lspg_query_push( NULL, qs);
  }
}

/** Request the values of all the I variables
 */
void lspmac_GetAllIVars() {
  static char *cmds = "I0..8191";
  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( cmds), cmds, lspmac_GetAllIVarsCB, 0, NULL);
}

/** Receive the values of all the M variables
 *  Update our database with the results
 */
void lspmac_GetAllMVarsCB(
			  pmac_cmd_queue_t *cmd,	/**< [in] The command that started this		*/
			  int nreceived,		/**< [in] Number of bytes received		*/
			  char *buff			/**< [in] Our byte buffer			*/
			  ) {
  static char qs[LS_PG_QUERY_STRING_LENGTH];
  char *sp;
  int i;
  for( i=0, sp=strtok(buff, "\r"); sp != NULL; sp=strtok( NULL, "\r"), i++) {
    snprintf( qs, sizeof( qs)-1, "SELECT pmac.md2_mvar_set( %d, '%s')", i, sp);
    qs[sizeof( qs)-1]=0;
    lspg_query_push( NULL, qs);
  }
}

/** Request the values of all the M variables
 */
void lspmac_GetAllMVars() {
  static char *cmds = "M0..8191->";
  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( cmds), cmds, lspmac_GetAllMVarsCB, 0, NULL);
}



/** Send a command that does not need to deal with the reply
 */
void lspmac_sendcmd_nocb(
			 char *fmt,		/**< [in] A printf style format string			*/
			 ...			/*        Arguments required by the format string	*/
			 ) {
  static char tmps[1024];
  va_list arg_ptr;

  va_start( arg_ptr, fmt);
  vsnprintf( tmps, sizeof(tmps)-1, fmt, arg_ptr);
  tmps[sizeof(tmps)-1]=0;
  va_end( arg_ptr);

  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen(tmps), tmps, NULL, 0, NULL);
}



/** PMAC command with call back
 */
void lspmac_sendcmd(
			char *event,						/**< [in] base name for events		       */
			void (*responseCB)(pmac_cmd_queue_t *, int, char *),	/**< [in] our callback routine                 */
			char *fmt,						/**< [in] printf style format string           */
			...							/*        Arguments specified by format string */
			) {
  static char tmps[1024];
  va_list arg_ptr;

  va_start( arg_ptr, fmt);
  vsnprintf( tmps, sizeof(tmps)-1, fmt, arg_ptr);
  tmps[sizeof(tmps)-1]=0;
  va_end( arg_ptr);

  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen(tmps), tmps, responseCB, 0, event);
}


/** State machine logic.
 *  Given the current state, generate the next one
 */
void lspmac_next_state() {


  //
  // Connect to the pmac and perhaps initialize it.
  // OK, this is slightly more than just the state
  // machine logic...
  //
  if( ls_pmac_state == LS_PMAC_STATE_DETACHED) {
    //
    // TODO (eventually)
    // This ip address wont change in a single PMAC installation
    // We'll need to audit the code if we decide to implement
    // multiple PMACs so might as well wait til then.
    //
    lsConnect( "192.6.94.5");

    //
    // If the connect was successful we can proceed with the initialization
    //
    if( ls_pmac_state != LS_PMAC_STATE_DETACHED) {
      lspmac_SockFlush();
      
      //
      // Harvest the I and M variables in case we need them
      // one day.
      //
      if( getmvars) {
	lspmac_GetAllMVars();
	getmvars = 0;
      }
      
      if( getivars) {
	lspmac_GetAllIVars();
	getivars = 0;
      }
    }
  }

  //
  // Check the command queue and perhaps go to the "Send Command" state.
  //
  if( ls_pmac_state == LS_PMAC_STATE_IDLE) {
    int goodtogo;
    goodtogo = 0;
    pthread_mutex_lock( &lspmac_ascii_mutex);
    if( lspmac_ascii_busy==0 && lspmac_dpascii_on != lspmac_dpascii_off)
      goodtogo = 1;
    pthread_mutex_unlock( &lspmac_ascii_mutex);
    if( goodtogo)
      lspmac_SockSendDPqueue();
  }

  if( ls_pmac_state == LS_PMAC_STATE_IDLE && ethCmdOn != ethCmdOff)
    ls_pmac_state = LS_PMAC_STATE_SC;


  //
  // Set the events flag
  // to tell poll what we are waiting for.
  //
  switch( ls_pmac_state) {
  case LS_PMAC_STATE_DETACHED:
    //
    // there shouldn't be a valid fd, so ignore the events
    //
    pmacfd.events = 0;
    break;

  case LS_PMAC_STATE_IDLE:
    if( ethCmdOn == ethCmdOff) {
      //
      // Anytime we are idle we want to
      // get the status of the PMAC
      //

      lspmac_get_status();
    }



  //
  // These states require that we listen for packets
  //
  case LS_PMAC_STATE_WACK_NFR:
  case LS_PMAC_STATE_WACK:
  case LS_PMAC_STATE_WACK_CC:
  case LS_PMAC_STATE_WACK_RR:
  case LS_PMAC_STATE_WCR:
  case LS_PMAC_STATE_WGB:
  case LS_PMAC_STATE_GMR:
    pmacfd.events = POLLIN;
    break;
    
  //
  // These states require that we send packets out.
  //
  case LS_PMAC_STATE_SC:
  case LS_PMAC_STATE_CR:
  case LS_PMAC_STATE_RR:
  case LS_PMAC_STATE_GB:
    //
    // Sad fact: PMAC will fail to process commands if we send them too quickly.
    // We deal with that by waiting a tad before we let poll tell us the PMAC socket is ready to write.
    //
    gettimeofday( &now, NULL);
    if(  ((now.tv_sec * 1000000. + now.tv_usec) - (pmac_time_sent.tv_sec * 1000000. + pmac_time_sent.tv_usec)) < PMAC_MIN_CMD_TIME) {
      pmacfd.events = 0;
    } else {
      pmacfd.events = POLLOUT;
    }
    break;
  }
}


/** Our lspmac worker thread.
 */
void *lspmac_worker(
		    void *dummy		/**< [in] Unused but required by pthread library		*/
		    ) {
  static int disconnected_notify = 0;
  static int old_state;

  old_state = ls_pmac_state;
  while( lspmac_running) {
    int pollrtn;

    lspmac_next_state();

    if( ls_pmac_state != old_state) {
      //      lslogging_log_message( "lspmac_worker: state = %d", ls_pmac_state);
      old_state = ls_pmac_state;
    }

    if( pmacfd.fd == -1) {
      if( disconnected_notify == 0)
	lslogging_log_message( "lspmac_worker: PMAC not connected");
      disconnected_notify = 1;
      //
      // At this point we assume we became disconnected due to something like a hard boot of the MD2 PMAC
      // and hence the entire system needs reinitialization.
      //
      // It's possible to put in a test here (perhaps using I65) to see if we in fact suffered a reset
      // and need to clear the queue, reinitialize, etc.  Or if it was just a networking glitch and do not
      // need to clear the queue and should instead just charge ahead.
      //
      lspmac_reset_queue();
      sleep( 10);
      //
      // This just puts us into a holding pattern until the pmac becomes connected again
      //
      continue;
    }
    disconnected_notify = 0;

    pollrtn = poll( &pmacfd, 1, 10);
    if( pollrtn) {
      lspmac_Service( &pmacfd);
    }
  }
  pthread_exit( NULL);
}





/** Move method for dac motor objects (ie, lights)
 */
int lspmac_movedac_queue(
			  lspmac_motor_t *mp,		/**< [in] Our motor						*/
			  double requested_position	/**< [in] Desired x postion (look up and send y position)	*/
			  ) {
  double u2c;

  pthread_mutex_lock( &(mp->mutex));

  u2c = lsredis_getd( mp->u2c);
  mp->requested_position = requested_position;

  if( mp->nlut > 0 && mp->lut != NULL) {
    //
    // u2c scales the lookup table value
    //
    mp->requested_pos_cnts = u2c * lspmac_lut( mp->nlut, mp->lut, requested_position);

    lslogging_log_message( "lspmac_movedac_queue: motor %s requested position %f  requested counts %d  u2c %f",
			   mp->name, mp->requested_position, mp->requested_pos_cnts, u2c);

    mp->not_done    = 1;
    mp->motion_seen = 0;

    lspmac_SockSendDPline( mp->name, "%s=%d", mp->dac_mvar, mp->requested_pos_cnts);
  }

  pthread_mutex_unlock( &(mp->mutex));
  return 0;
}


/** Move method for the zoom motor
 */
int lspmac_movezoom_queue(
			   lspmac_motor_t *mp,			/**< [in] the zoom motor		*/
			   double requested_position		/**< [in] our desired zoom		*/
			   ) {
  int motor_num;
  int in_position_band;

  lslogging_log_message( "lspmac_movezoom_queue: Here I am");
  pthread_mutex_lock( &(mp->mutex));

  motor_num        = lsredis_getl( mp->motor_num);
  in_position_band = lsredis_getl( mp->in_position_band);

  mp->requested_position = requested_position;

  if( mp->nlut > 0 && mp->lut != NULL) {
    mp->requested_pos_cnts = lspmac_lut( mp->nlut, mp->lut, requested_position);

    if( abs( mp->requested_pos_cnts - mp->actual_pos_cnts) * 16 <= in_position_band) {
      lslogging_log_message( "lspmac_movezoom_queue: Faking move");
      //
      // fake the move
      //
      mp->not_done     = 1;
      mp->motion_seen  = 0;
      mp->command_sent = 1;
      pthread_mutex_unlock( &(mp->mutex));
      lsevents_send_event( "%s Moving", mp->name);

      //
      // Perhaps give someone else a chance to process the move
      //
      pthread_mutex_lock( &(mp->mutex));
      mp->not_done     = 0;
      mp->motion_seen  = 1;
      mp->command_sent = 1;
      pthread_mutex_unlock( &(mp->mutex));
      lsevents_send_event( "%s In Position", mp->name);
      return 0;
    }

    mp->not_done     = 1;
    mp->motion_seen  = 0;
    mp->command_sent = 0;
    
    lspmac_SockSendDPline( mp->name, "#%d j=%d", motor_num, mp->requested_pos_cnts);
  }
  pthread_mutex_unlock( &(mp->mutex));
  lslogging_log_message( "lspmac_movezoom_queue: There you were");
  return 0;
}

/** Move a given motor to one of its preset positions.
 *  No movement if the preset is not found.
 *  \param             mp lspmac motor pointer
 *  \param preset_name Name of the preset to use
 */
int lspmac_move_preset_queue( lspmac_motor_t *mp, char *preset_name) {
  double pos;
  int err;

  lslogging_log_message( "lspmac_move_preset_queue: Called with motor %s and preset named '%s'", mp->name, preset_name);

  err = lsredis_find_preset( mp->name, preset_name, &pos);
  if( err == 0)
    return 1;

  err = mp->jogAbs( mp, pos);
  if( !err)
    lslogging_log_message( "lspmac_move_preset_queue: moving %s to preset '%s' (%f)", mp->name, preset_name, pos);
  //
  // the abort event should have been sent in moveAbs
  //
  return err;
}

/** see if the motor is within tolerance of the preset
 *   1 means yes, it is
 *   0 mean  no it isn't or that the preset was not found
 */
int lspmac_test_preset( lspmac_motor_t *mp, char *preset_name, double tolerance) {
  double preset_position;
  int err;

  err = lsredis_find_preset( mp->name, preset_name, &preset_position);
  if( err == 0)
    return 0;

  if( fabs( preset_position - lspmac_getPosition( mp)) <= tolerance)
    return 1;

  return 0;
}



/** Move method for the fast shutter
 *
 *  Slightly more complicated than a binary io as some flags need
 *  to be set up.
 */
int lspmac_moveabs_fshut_queue(
				lspmac_motor_t *mp,		/**< The fast shutter motor instance				*/
				double requested_position	/**< 1 (open) or 0 (close), really				*/
				) {
  pthread_mutex_lock( &(mp->mutex));

  mp->requested_position = requested_position;
  mp->not_done    = 1;
  mp->motion_seen = 0;
  mp->requested_pos_cnts = requested_position;
  if( requested_position != 0) {
    //
   // ScanEnable=0, ManualEnable=1, ManualOn=1
    //
    lspmac_SockSendDPline( mp->name, "M1124=0 M1125=1 M1126=1");
  } else {
    //
    // ManualOn=0, ManualEnable=0, ScanEnable=0
    //
    lspmac_SockSendDPline( mp->name, "M1126=0 M1125=0 M1124=0");
  }

  pthread_mutex_unlock( &(mp->mutex));

  return 0;

}

/** Move method for binary i/o motor objects
 */
int lspmac_moveabs_bo_queue(
			      lspmac_motor_t *mp,		/**< [in] A binary i/o motor object		*/
			      double requested_position		/**< [in] a 1 or a 0 request to move		*/
			      ) {


  pthread_mutex_lock( &(mp->mutex));
  mp->requested_position = requested_position == 0.0 ? 0.0 : 1.0;
  mp->requested_pos_cnts = requested_position == 0.0 ? 0 : 1;

  if( mp->requested_position == mp->position) {
    //
    // No real move requested
    //
    mp->not_done     = 0;
    mp->motion_seen  = 1;
    mp->command_sent = 1;
    lsevents_send_event( "%s Moving", mp->name);
    lsevents_send_event( "%s In Position", mp->name);

  } else {
    //
    // Go ahead and send the request
    //
    mp->not_done     = 1;
    mp->motion_seen  = 0;
    mp->command_sent = 0;
    lspmac_SockSendDPline( mp->name, mp->write_fmt, mp->requested_pos_cnts);
  }

  pthread_mutex_unlock( &(mp->mutex));
  return 0;
}


/** timed motor move
 *  \param mp Our motor object
 *  \param start Beginning of motion
 *  \param delta Distance to move
 *  \param time to move it in (secs)
 */
void lspmac_moveabs_timed_queue(
				lspmac_motor_t *mp,
				double start,
				double delta,
				double time
				) {
  // 240		LS-CAT Timed X move
  //		Q10    = Starting X value (cnts)
  //		Q11    = Delta X value   (cnts)
  //		Q12    = Time to run between the two points (mSec)
  //		Q13    = Acceleration time (msecs)
  //		Q100   = 1 << (coord sys no - 1)

  int q10;	 // Starting value (counts)
  int q11;	 // Delta (counts)
  int q12;	 // Time to run (msecs)
  int q13;	 // Acceleration time (msecs)
  int q100;	 // 1 << (coord sys no - 1)
  int coord_num; // our coordinate number
  double u2c;
  double neutral_pos;
  double max_accel;

  pthread_mutex_lock( &(mp->mutex));

  u2c         = lsredis_getd( mp->u2c);
  max_accel   = lsredis_getd( mp->max_accel);
  coord_num   = lsredis_getl( mp->coord_num);
  neutral_pos = lsredis_getd( mp->neutral_pos);

  if( u2c == 0.0 || time <= 0.0 || max_accel <= 0.0) {
    //
    // Shouldn't try moving a motor that has bad motion parameters
    //
    pthread_mutex_unlock( &(mp->mutex));
    return;
  }

  mp->not_done    = 1;		//!< Flags needed for wait routine
  mp->motion_seen = 0;

  mp->requested_position = start + delta;
  mp->requested_pos_cnts = u2c * (mp->requested_position + neutral_pos);
  q10 = mp->requested_pos_cnts;
  q11 = u2c * delta;
  q12 = 1000 * time;
  q13 = q11 / q12 / max_accel;
  q100 = 1 << (coord_num - 1);
  pthread_mutex_unlock( &(mp->mutex));

  pthread_mutex_lock( &(mp->mutex));
  lspmac_SockSendDPline( mp->name, "&%d Q10=%d Q11=%d Q12=%d Q13=%d Q100=%d B240R", coord_num, q10, q11, q12, q13, q100);
  pthread_mutex_unlock( &(mp->mutex));
}

/** "move" frontlight on/off
 */
int lspmac_moveabs_frontlight_oo_queue( lspmac_motor_t *mp, double pos) {
  pthread_mutex_lock( &(mp->mutex));
  *mp->actual_pos_cnts_p = pos;
  mp->position =           pos;
  pthread_mutex_unlock( &(mp->mutex));
  if( pos == 0.0) {
    flight->moveAbs( flight, 0.0);
  } else {
    flight->moveAbs( flight, lspmac_getPosition( zoom));
  }
  return 0;
}

int lspmac_moveabs_flight_factor_queue( lspmac_motor_t *mp, double pos) {
  char *fmt;

  if( pos >= 60 && pos <= 140) {
    pthread_mutex_lock( &(mp->mutex));
    *mp->actual_pos_cnts_p = pos;
    mp->position =           pos;
    pthread_mutex_unlock( &(mp->mutex));

    pthread_mutex_lock( &(flight->mutex));

    fmt = lsredis_getstr( flight->redis_fmt);
    lsredis_setstr( flight->u2c, fmt, pos / 100.0);
    free( fmt);

    pthread_mutex_unlock( &(flight->mutex));

    flight->moveAbs( flight, lspmac_getPosition( zoom));
    return 0;
  }
  return 1;
}

int lspmac_moveabs_blight_factor_queue( lspmac_motor_t *mp, double pos) {
  char *fmt;

  if( pos >= 60 && pos <= 140) {
    pthread_mutex_lock( &(mp->mutex));
    *mp->actual_pos_cnts_p = pos;
    mp->position =           pos;
    pthread_mutex_unlock( &(mp->mutex));

    pthread_mutex_lock( &(blight->mutex));
    fmt = lsredis_getstr( blight->redis_fmt);
    lsredis_setstr( blight->u2c, fmt, pos / 100.0);
    free( fmt);
    pthread_mutex_unlock( &(blight->mutex));

    blight->moveAbs( blight, lspmac_getPosition( zoom));
  }

  return 0;
}


/** Special motion program to collect centering video
 */
void lspmac_video_rotate( double secs) {
  double q10;		// starting position (counts)
  double q11;		// delta counts
  double q12;		// milliseconds to run over delta
  
  double u2c;
  double neutral_pos;

  if( secs <= 0.0)
    return;

  omega_zero_search = 1;

  pthread_mutex_lock( &(omega->mutex));
  u2c         = lsredis_getd( omega->u2c);
  neutral_pos = lsredis_getd( omega->neutral_pos);

  q10 = neutral_pos * u2c;
  q11 = 360.0 * u2c;
  q12 = 1000 * secs;
  

  omega_zero_velocity = 360.0 * u2c / secs;	// counts/second to back calculate zero crossing time

  lspmac_SockSendDPline( omega->name, "&1 Q10=%.1f Q11=%.1f Q12=%.1f Q13=(I117) Q14=(I116) B240R", q10, q11, q12);
  pthread_mutex_unlock( &(omega->mutex));
}



/** Set the coordinate system motion flags (m5075)
 *  for the null terminated list of motors that we are planning
 *  on running a motion program with.  Note that lspmac_est_move_time
 *  already takes care of this, use when calling a motion program directly
 *
 * \param mmaskp Returned value of the mask generated.  Ignored if null.
 * \param mp_1 start of null terminated list of motors.
 */
int lspmac_set_motion_flags( int *mmaskp, lspmac_motor_t *mp_1, ...) {
  va_list arg_ptr;
  struct timespec timeout;
  int err;
  int cn;
  int need_flag;
  lspmac_motor_t *mp;
  int mmask;

  mmask = 0;
  if( mmaskp != NULL)
    *mmaskp = 0;

  if( mp_1==NULL)
    return 0;
  
  
  //
  // add the coordinate system flags to mmask
  //
  va_start( arg_ptr, mp_1);
  for( mp = mp_1; mp!=NULL; mp = va_arg( arg_ptr, lspmac_motor_t *)) {
    if( mp->magic != LSPMAC_MAGIC_NUMBER) {
      lslogging_log_message( "lspmac_set_motion_flags: WARNING: motor list must be NULL terminated.  Check your call to lspmac_set_motion_flags.");
      break;
    }
    cn = lsredis_getl( mp->coord_num);
    if( cn < 1 || cn > 16)
      continue;
    
    mmask |= 1 << (cn - 1);
  }
  va_end( arg_ptr);

  if( mmaskp != NULL)
    *mmaskp = mmask;
    
  //
  // It could be the flag is already what we want.  We might set up a race condition if we
  // try to set it again.  (so don't)
  //
  pthread_mutex_lock( &lspmac_moving_mutex);

  if( (lspmac_moving_flags & mmask) != 0)
    need_flag = 0;
  else
    need_flag = 1;
  
  pthread_mutex_unlock( &lspmac_moving_mutex);

  if( !need_flag)
    return 0;
  
  //
  // Set m5075 and make sure it propagates
  //
  lspmac_SockSendDPline( NULL, "M5075=(M5075 | %d)", mmask);
  clock_gettime( CLOCK_REALTIME, &timeout);
  timeout.tv_sec += 2;

  err = 0;
  pthread_mutex_lock( &lspmac_moving_mutex);
  while( err == 0 && (lspmac_moving_flags & mmask) != mmask)
    err = pthread_cond_timedwait( &lspmac_moving_cond, &lspmac_moving_mutex, &timeout);

  pthread_mutex_unlock( &lspmac_moving_mutex);
  
  if( err == ETIMEDOUT) {
    lslogging_log_message( "lspmac_set_motion_flags: timed out waiting for motion %d flag to be set", mmask);
    return 1;
  }
  return 0;
}


/** Move the motors and estimate the time it'll take to finish the job.
 * Returns the estimate time and the coordinate system mask to waite for
 * \param est_time     Returns number of seconds we estimate the move(s) will take
 * \param mmaskp       Mask of coordinate systems we are trying to move, excluding jogs.  Used to wait for motions to complete
 * \param mp_1         Pointer to first motor
 * \param jog_1        1 to force a jog, 0 to try a motion program  DO NOT MIX JOGS AND MOTION PROGRAMS IN THE SAME COORDINATE SYSTEM!
 * \param preset_1     Name of preset we'd like to move to or NULL if end_point_1 should be used instead
 * \param end_point_1  End point for the first motor.  Ignored if preset_1 is non null and identifies a valid preset for this motor
 * \param ...          Perhaps more quads of motors, jog flags, preset names, and end points.  End is a NULL motor pointer
 * MUST END ARG LIST WITH NULL
 */
int lspmac_est_move_time( double *est_time, int *mmaskp, lspmac_motor_t *mp_1, int jog_1, char *preset_1, double end_point_1, ...) {
  static char axes[] = "XYZUVWABC";
  int qs[9];
  lspmac_combined_move_t motions[32];
  char s[256];
  int foundone;
  int moving_flags;
  struct timespec timeout;
  int j;
  va_list arg_ptr;
  lspmac_motor_t *mp;
  double ep, maybe_ep;
  char *ps;
  double
    min_pos,
    max_pos,
    neutral_pos,
    u2c,		//!< units to counts
    D,			//!< The total distance we need to go
    V,			//!< Our maximum velocity
    A,			//!< Our maximum acceleration
    Tt;                 //!< Total time for this motor
  int err;
  int jog;
  int i;
  uint32_t m5075;		//!< coordinate system motion flags

  // reset our coordinate flags and command strings
  //
  for( i=0; i<32; i++) {
    motions[i].moveme = 0;
  }
  m5075  = 0;
  if( mmaskp != NULL)
    *mmaskp = 0;

  //
  // Initialze first iteration
  //
  *est_time = 0.0;
  mp  = mp_1;
  ps  = preset_1;
  ep  = end_point_1;
  jog = jog_1;

  va_start( arg_ptr, end_point_1);
  while( 1) {
    /*
     *    :                  |       Constant       |
     *    :                  |<---   Velocity   --->|
     *    :                  |       Time (Ct)      |
     *  V :                   ----------------------              ---------
     *  e :                 /                        \               ^
     *  l :                /                          \              |
     *  o :               /                            \             |
     *  c :              /                              \            V
     *  i :             /                                \           |
     *  t :            /                                  \          |
     *  y :___________/....................................\_________v___________
     *                |      |         Time              
     *                |      |
     *             -->|      |<-- Acceleration Time  (At)
     *                |
     *                |<-----    Total  Time (Tt)  ------->|
     *
     *      Assumption 1: We can replace S curve acceleration with linear acceleration
     *      for the purposes of distance and time calculations for the timeout
     *      period that we are attempting to calculate here.
     *
     *      Ct  = Constant Velocity Time.  The time spent at constant velocity.
     *
     *      At  = Acceleration Time.  Time spent accelerating at either end of the ramp, that is,
     *      1/2 the total time spent accelerating and decelerating.
     *
     *      D   = the total distance we need to travel
     *
     *      V   = constant velocity.  Here we use the motor's maximum velocity.
     *
     *      A   = the motor acceleration, Here it's the maximum acceleration.
     *
     *      V = A * At   
     *
     *      or  At = V/A
     *
     *      The Total Time (Tt) is
     *
     *      Tt = Ct + 2 * At
     *
     *
     *
     *      If we had infinite acceleration the total time would be D/V.  To account for finite acceleration we just need to
     *      adjust this for the average velocity while accelerating (0.5 V).  This neatly adds a single V/A term:
     *
     *      (1)     Tt = D/V  + V/A
     *
     *      When the distance is short, we need a different calculation:
     *
     *      D = 0.5 * A * T1^2  + 0.5 * A * T2^2  (T1 = acceleration time and T2 = deceleration time)
     *
     *      or, since total time  Tt = T1 + T2 and T1 = T2,
     *
     *      D = A * (0.5*Tt)^2
     *
     *      or
     *      
     *      (2)    Tt = 2 * sqrt( D/A)
     *
     *
     *      When we accelerate to the maximum speed the time it takes is V/A so the distance we travel (Da) is
     *
     *      Da = 0.5 * A * (V/A)^2
     *
     *      or
     *
     *      Da = 0.5 * V^2 / A
     *
     *      So when D > 2 * Da, or
     *
     *      D > V^2 / A
     *         
     *      we need to use equation (1) otherwise we need to use equation (2)
     *
     */

    if( mp->magic != LSPMAC_MAGIC_NUMBER) {
      lslogging_log_message( "lspmac_est_move_time: WARNING: bad motor structure.  Check that your motor list is NULL terminated.");
      break;
    }


    lslogging_log_message( "lspmac_est_move_time: find motor %s, jog %d, preset %s, endpoint %f",
			   mp->name, jog, ps == NULL ? "NULL" : ps, ep);

    Tt = 0.0;
    if( mp != NULL && mp->max_speed != NULL && mp->max_accel != NULL && mp->u2c != NULL) {

      //
      // get the real endpoint if a preset was mentioned
      //
      if( ps != NULL && *ps != 0) {
	err = lsredis_find_preset( mp->name, ps, &maybe_ep);
	if( err != 0)
	  ep = maybe_ep;
      }

      u2c = lsredis_getd( mp->u2c);

      //
      // For look up tables user units are (or should be) counts and u2c should be 1
      //
      if( mp->nlut > 0 && mp->lut != NULL) {
	u2c = 1.0;
	D = lspmac_lut( mp->nlut, mp->lut, ep) - lspmac_lut( mp->nlut, mp->lut, lspmac_getPosition( mp));
      } else {
	D = ep - lspmac_getPosition( mp);				// User units
      }

      V = lsredis_getd( mp->max_speed) / u2c * 1000.;		// User units per second
      A = lsredis_getd( mp->max_accel) / u2c * 1000. * 1000;	// User units per second per second


      neutral_pos = lsredis_getd( mp->neutral_pos);
      min_pos     = lsredis_getd( mp->min_pos) - neutral_pos;
      max_pos     = lsredis_getd( mp->max_pos) - neutral_pos;

      if( ep < min_pos || ep > max_pos) {
	lslogging_log_message( "lspmac_est_move_time: Motor %s Requested position %f out of range: min=%f, max=%f", mp->name, ep, min_pos, max_pos);
	lsevents_send_event( "%s Move Aborted", mp->name);
	return 1;
      }

      mp->requested_position = ep;
      mp->requested_pos_cnts = u2c * (mp->requested_position + neutral_pos);


      //
      // Don't bother with motors without velocity or acceleration defined
      //
      if( V > 0.0 && A > 0.0) {
	if( fabs(D) > V*V/A) {
	  //
	  // Normal ramp up, constant velocity, and ramp down
	  //
	  Tt = fabs(D)/V + V/A;
	} else {
	  //
	  // Never reach constantant velocity, just ramp up a bit and back down
	  //
	  Tt = 2.0 * sqrt( fabs(D)/A);
	}

	lslogging_log_message( "lspmac_est_move_time: Motor: %s  D: %f  VV/A: %f  Tt: %f", mp->name, D, V*V/A, Tt);
      }  else {
	//
	// TODO: insert move time based for DAC or BO motor like objects;
	// For now assume 100 msec;
	//
	Tt = 0.1;
      }

      // Perhaps flag a coordinate system
      //
      // We can move a motor that's not in a coordinate system but we cannot move a motor that is but does not
      // have an axis defined if we are also moving one that does.  It's a limitation, I guess.
      //
      if( jog != 1 &&
	  mp->coord_num != NULL && lsredis_getl( mp->coord_num) > 0 && lsredis_getl( mp->coord_num) <= 16 &&
	  mp->motor_num != NULL && lsredis_getl( mp->motor_num) > 0 && mp->axis != NULL && lsredis_getc( mp->axis) != 0) {
	int axis;
	int motor_num;

	motor_num = lsredis_getl( mp->motor_num);

	axis = lsredis_getc( mp->axis);
	for( j=0; j<sizeof(axes); j++) {
	  if( axis == axes[j])
	    break;
	}

	if( j < sizeof( axes)) {
	  //
	  // Store the motion request for a normal PMAC motor
	  //
	  int cn;
	  int in_position_band;

	  cn = lsredis_getl( mp->coord_num);
	  in_position_band = lsredis_getl( mp->in_position_band);

	  motions[motor_num - 1].coord_num = cn;
	  motions[motor_num - 1].axis      = j;
	  motions[motor_num - 1].Delta     = D * u2c;
	  //
	  // Don't ask to run a motion program if we are already where we want to be
	  //
	  // Deadband is 10 counts except for zoom which is 100.
	  // We use Ixx28 In-Position Band which has units of 1/16 count
	  //
	  //
	  if( abs(motions[motor_num - 1].Delta)*16 >= in_position_band) {
	    m5075 |= (1 << (cn - 1));
	    motions[motor_num - 1].moveme    = 1;
	  }	  
	  lslogging_log_message( "lspmac_est_move_time: moveme=%d  motor '%s' motions index=%d coord_num=%d axis=%d Delta=%d   m5075=%u",
				 motions[motor_num-1].moveme,  mp->name, motor_num -1, motions[motor_num-1].coord_num, motions[motor_num-1].axis, motions[motor_num-1].Delta,
				 m5075);
	}
      } else {
	//
	// Here we are dealing with a DAC or BO motor or just want to jog.
	//
	if( mp->jogAbs( mp, ep)) {
	  lslogging_log_message( "lspmac_est_move_time: motor %s failed to queue move of distance %f from %f", mp->name, D, lspmac_getPosition(mp));
	  lsevents_send_event( "Move Aborted");
	  return 1;
	}
      }
      //
      // Update the estimated time
      //
      *est_time = *est_time < Tt ? Tt : *est_time;
      
      lslogging_log_message( "lspmac_est_move_time: est_time=%f", *est_time);

    }


    mp = va_arg( arg_ptr, lspmac_motor_t *);
    if( mp == NULL)
      break;

    jog = va_arg( arg_ptr, int);
    ps  = va_arg( arg_ptr, char *);
    ep  = va_arg( arg_ptr, double);

  }
  va_end( arg_ptr);

  
  // Set the motion program flags
  //
  if( m5075 != 0) {
    if( mmaskp != NULL)
      *mmaskp |= m5075;	// Tell the caller about our new mask

    
    pthread_mutex_lock( &lspmac_moving_mutex);
    moving_flags = lspmac_moving_flags;
    pthread_mutex_unlock( &lspmac_moving_mutex);

    if( (moving_flags & m5075) != m5075) {
      lspmac_SockSendDPline( NULL, "M5075=(M5075 | %d)", m5075);
    

      pthread_mutex_lock( &lspmac_moving_mutex);
      clock_gettime( CLOCK_REALTIME, &timeout);
      //
      timeout.tv_sec += 2;	// 2 seconds should be more than enough time to set the flags
      err = 0;
      while( err == 0 && ((lspmac_moving_flags & m5075) != m5075))
	err = pthread_cond_timedwait( &lspmac_moving_cond, &lspmac_moving_mutex, &timeout);
      moving_flags = lspmac_moving_flags;
      pthread_mutex_unlock( &lspmac_moving_mutex);
    
      if( ((moving_flags & m5075) != m5075) && err == ETIMEDOUT) {
	lslogging_log_message( "lspmac_est_move_time: Timed out waiting for moving flags.  lspmac_moving_flags = 0x%0x, looking for 0x%0x  test exp: 0x%0x  test: %d",
			       moving_flags, m5075, (moving_flags & m5075), (moving_flags & m5075) != m5075);
	lsevents_send_event( "Combined Move Aborted");
	return 1;
      }
    }
  }


  for( i=1; i<=16; i++) {
    //
    // Loop over coordinate systems
    //
    foundone = 0;
    
    for( j=0; j<9; j++)
      qs[j] = 0;
    
    for( j=0; j<31; j++) {
      //
      // Loop over motors
      //
      if( motions[j].moveme && motions[j].coord_num == i) {
	if( abs(motions[j].Delta) > 0) {
	  qs[(int)(motions[j].axis)] = motions[j].Delta;
	  foundone=1;
	}
      }
    }
    
    if( foundone) {
      sprintf( s, "&%d Q40=%d Q41=%d Q42=%d Q43=%d Q44=%d Q45=%d Q46=%d Q47=%d Q48=%d Q49=%.1f Q100=%d B180R",
	       i, qs[0], qs[1], qs[2], qs[3], qs[4], qs[5], qs[6], qs[7], qs[8], *est_time * 1000., 1 << (i-1));
      
      lspmac_SockSendDPline( NULL, s);
      
    }
  }
  return 0;
}


/** wait for motion to stop
 * returns non-zero if the wait timed out
 * \param move_time The time out in seconds
 * \param cmask     A coordinate system mask to wait for
 * \param mp_1      NULL terminated list of individual motors to wait for
 *
 * Both values are returned from lspmac_est_move_time
 */
int lspmac_est_move_time_wait( double move_time, int cmask, lspmac_motor_t *mp_1, ...) {
  int err;
  double isecs, fsecs;
  struct timespec timeout;
  va_list arg_ptr;
  lspmac_motor_t *mp;

  clock_gettime( CLOCK_REALTIME, &timeout);
  fsecs = modf( move_time, &isecs);
  timeout.tv_sec  += (long)floor(isecs);
  timeout.tv_nsec += (long)floor(fsecs * 1.e9);
  timeout.tv_sec  += timeout.tv_nsec / 1000000000;
  timeout.tv_nsec %= 1000000000;

  err = 0;
  pthread_mutex_lock( &lspmac_moving_mutex);
  while( err == 0 && (lspmac_moving_flags & cmask) != 0)
    err = pthread_cond_timedwait( &lspmac_moving_cond, &lspmac_moving_mutex, &timeout);
  pthread_mutex_unlock( &lspmac_moving_mutex);

  if( err != 0) {
    if( err == ETIMEDOUT) {
      lslogging_log_message( "lstest_lspmac_est_move_time_wait: timed out waiting %f seconds, cmask = 0x%0x", move_time, cmask);
    }
    lspmac_abort();
    return 1;
  }

  va_start( arg_ptr, mp_1);
  for( mp = mp_1; mp != NULL; mp = va_arg( arg_ptr, lspmac_motor_t *)) {
    if( mp->magic != LSPMAC_MAGIC_NUMBER) {
      lslogging_log_message( "lspmac_est_move_time_wait: WARNING: motor list must be NULL terminated.  Check your call to lspmac_est_move_time_wait.");
    }

    if( lspmac_moveabs_wait( mp, move_time)) {
      lslogging_log_message( "lspmac_est_move_time_wait: timed out waiting %f seconds for motor %s", move_time, mp->name);
      return 1;
    }
  }
  va_end( arg_ptr);

  return 0;
}





/** Move method for normal stepper and servo motor objects
 *  Returns non-zero on abort, zero if OK
 */
int lspmac_move_or_jog_abs_queue(
				 lspmac_motor_t *mp,			/**< [in] The motor to move			*/
				 double requested_position,		/**< [in] Where to move it			*/
				 int use_jog				/**< [in] 1 to force jog, 0 for motion prog	*/
				 ) {
  char *fmt;			//!< format string for coordinate system move
  int q100;			//!< coordinate system bit
  int requested_pos_cnts;	//!< the requested position in units of "counts"
  int coord_num, motor_num;	//!< motor and coordinate system;
  char *axis;			//!< our axis
  double u2c;
  double neutral_pos;
  double min_pos, max_pos;
  int pos_limit_hit, neg_limit_hit, in_position_band;
  struct timespec timeout, now;
  int err;

  pthread_mutex_lock( &(mp->mutex));

  u2c              = lsredis_getd(   mp->u2c);
  motor_num        = lsredis_getl(   mp->motor_num);
  coord_num        = lsredis_getl(   mp->coord_num);
  axis             = lsredis_getstr( mp->axis);
  neutral_pos      = lsredis_getd(   mp->neutral_pos);
  min_pos          = lsredis_getd(   mp->min_pos) - neutral_pos;
  max_pos          = lsredis_getd(   mp->max_pos) - neutral_pos;
  pos_limit_hit    = lsredis_getd(   mp->pos_limit_hit);
  neg_limit_hit    = lsredis_getd(   mp->neg_limit_hit);
  in_position_band = lsredis_getl(   mp->in_position_band);

  if( u2c == 0.0 || requested_position < min_pos || requested_position > max_pos) {
    //
    // Shouldn't try moving a motor that's in trouble
    //
    pthread_mutex_unlock( &(mp->mutex));
    lslogging_log_message( "lspmac_move_or_jog_abs_queue: %s  u2c=%f  requested position=%f  min allowed=%f  max allowed=%f", mp->name, u2c, requested_position, min_pos, max_pos);
    lsevents_send_event( "%s Move Aborted", mp->name);
    return 1;
  }

  if( (neg_limit_hit && (requested_position < mp->position)) || (pos_limit_hit && (requested_position > mp->position))) {
    pthread_mutex_unlock( &(mp->mutex));
    lslogging_log_message( "lspmac_move_or_jog_abs_queue: %s Moving wrong way on limit: requested position=%f  current position=%f  low limit=%d high limit=%d",
			   mp->name, requested_position, mp->position, neg_limit_hit, pos_limit_hit);
    lsevents_send_event( "%s Move Aborted", mp->name);
    return 2;
  }


  mp->requested_position = requested_position;
  if( mp->nlut > 0 && mp->lut != NULL) {
    mp->requested_pos_cnts = lspmac_lut( mp->nlut, mp->lut, requested_position);
  } else {
    mp->requested_pos_cnts = u2c * (requested_position + neutral_pos);
  }
  requested_pos_cnts = mp->requested_pos_cnts;

  //
  // Bluff if we are already there
  //
  if( (abs( requested_pos_cnts - mp->actual_pos_cnts) * 16 < in_position_band) || (lsredis_getb( mp->active) != 1)) {
    //
    // Lie and say we moved even though we didn't.  Who will know? We are within the deadband or not active.
    //
    mp->not_done     = 1;
    mp->motion_seen  = 0;
    mp->command_sent = 0;

    lsevents_send_event( "%s Moving", mp->name);

    mp->not_done     = 0;
    mp->motion_seen  = 1;
    mp->command_sent = 1;
    


    if( lsredis_getb( mp->active) != 1) {
      //
      // fake the motion for simulated motors
      //
      mp->position = requested_position;
      mp->actual_pos_cnts = requested_pos_cnts;
    }
    pthread_mutex_unlock( &(mp->mutex));

    lsevents_send_event( "%s In Position", mp->name);
    return 0;
  }

  mp->not_done     = 1;
  mp->motion_seen  = 0;
  mp->command_sent = 0;


  if( use_jog || axis == NULL || *axis == 0) {
    use_jog = 1;
  } else {
    use_jog = 0;
    q100 = 1 << (coord_num -1);
  }


  pthread_mutex_unlock( &(mp->mutex));

  if( !use_jog) {
    //
    // Make sure the coordinate system is not moving something, wait if it is
    //
    pthread_mutex_lock( &lspmac_moving_mutex);

    clock_gettime( CLOCK_REALTIME, &now);
    //
    // TODO: Have all moves estimate how long they'll take and use that here
    //
    timeout.tv_sec  = now.tv_sec + 60.0;		// a long timeout, but we might really be moving something that takes this long (or longer)
    timeout.tv_nsec = now.tv_nsec;

    err = 0;
    while( err == 0 &&  (lspmac_moving_flags & q100) != 0)
      err = pthread_cond_timedwait( &lspmac_moving_cond, &lspmac_moving_mutex, &timeout);
    
    pthread_mutex_unlock( &lspmac_moving_mutex);

    if( err == ETIMEDOUT) {
      lslogging_log_message( "lspmac_move_or_jog_abs_queue: Timed Out.  lspmac_moving_flags = %0x", lspmac_moving_flags);
      lsevents_send_event( "%s Move Aborted", mp->name);
      return 1;
    }

    //
    // Set the "we are moving this coordinate system" flag
    //
    lspmac_SockSendDPline( NULL, "M5075=(M5075 | %d)", q100);
    
    switch( *axis) {
    case 'A':
      fmt = "&%d Q16=%d Q100=%d B146R";
      break;

    case 'B':
      fmt = "&%d Q17=%d Q100=%d B147R";
      break;

    case 'C':
      fmt = "&%d Q18=%d Q100=%d B148R";
      break;
    case 'X':
      fmt = "&%d Q10=%d Q100=%d B140R";
      break;

    case 'Y':
      fmt = "&%d Q11=%d Q100=%d B141R";
      break;

    case 'Z':
      fmt = "&%d Q12=%d Q100=%d B142R";
      break;

    case 'U':
      fmt = "&%d Q13=%d Q100=%d B143R";
      break;

    case 'V':
      fmt = "&%d Q14=%d Q100=%d B144R";
      break;

    case 'W':
      fmt = "&%d Q15=%d Q100=%d B145R";
      break;
    }

    //
    // Make sure the flag has been seen
    //

    clock_gettime( CLOCK_REALTIME, &now);
    timeout.tv_sec  = now.tv_sec + 4.0;		// also a long timeout.  This should really only take a few milliseconds on a slow day
    timeout.tv_nsec = now.tv_nsec;

    pthread_mutex_lock( &lspmac_moving_mutex);

    err = 0;
    while( err == 0 && (lspmac_moving_flags & q100) == 0)
      err = pthread_cond_timedwait( &lspmac_moving_cond, &lspmac_moving_mutex, &timeout);
    pthread_mutex_unlock( &lspmac_moving_mutex);

    if( err == ETIMEDOUT) {
      lslogging_log_message( "lspmac_move_or_jog_abs_queue: Did not see flag propagate.  Move aborted.");
      lsevents_send_event( "%s Move Aborted", mp->name);
      return 1;
    }
  }

  pthread_mutex_lock( &(mp->mutex));
  if( use_jog) {
    lspmac_SockSendDPline( mp->name, "#%d j=%d", motor_num, requested_pos_cnts);
  } else {
    lspmac_SockSendDPline( mp->name, fmt, coord_num, requested_pos_cnts, q100);
  }
  pthread_mutex_unlock( &(mp->mutex));

  free( axis);

  return 0;
}

/** move using a preset value
 *  returns 0 on success, non-zero on error
 */
int lspmac_move_or_jog_preset_queue(
				     lspmac_motor_t *mp,	/**< [in] Our motor				*/
				     char *preset,		/**< [in] the name of the preset		*/
				     int use_jog		/**< [in[ 1 to force jog, 0 to try motion prog	*/
				     ) {
  double pos;
  int err;
  int rtn;

  if( preset == NULL || *preset == 0) {
    lsevents_send_event( "%s Move Aborted", mp->name);
    return 0;
  }

  err = lsredis_find_preset( mp->name, preset, &pos);

  if( err != 0)
    rtn = lspmac_move_or_jog_abs_queue( mp, pos, use_jog);
  else {
    lsevents_send_event( "%s Move Aborted", mp->name);
    rtn = 1;
  }
  return rtn;
}


/** Use coordinate system motion program, if available, to move motor to requested position
 */
int lspmac_moveabs_queue(
			  lspmac_motor_t *mp,			/**< [in] The motor to move			*/
			  double requested_position		/**< [in] Where to move it			*/
			  ) {
  
  return lspmac_move_or_jog_abs_queue( mp, requested_position, 0);
}


/** Use jog to move motor to requested position
 */
int lspmac_jogabs_queue(
			  lspmac_motor_t *mp,			/**< [in] The motor to move			*/
			  double requested_position		/**< [in] Where to move it			*/
			  ) {
  
  return lspmac_move_or_jog_abs_queue( mp, requested_position, 1);
}


/** Wait for motor to finish moving.
 *  Assume motion already queued, now just wait
 *
 *  \param mp The motor object to wait for
 *  \param timeout_secs  The number of seconds to wait for.  Fractional values fine.
 */
int lspmac_moveabs_wait( lspmac_motor_t *mp, double timeout_secs) {
  struct timespec timeout, now;
  double isecs, fsecs;
  int err;

  //
  // Copy the queue item for the most recent move request
  //
  clock_gettime( CLOCK_REALTIME, &now);

  fsecs = modf( timeout_secs, &isecs);

  timeout.tv_sec  = now.tv_sec  + (long)floor( isecs);
  timeout.tv_nsec = now.tv_nsec + (long)floor( fsecs * 1.0e9);
  
  timeout.tv_sec  += timeout.tv_nsec / 1000000000;
  timeout.tv_nsec %= 1000000000;

  err = 0;
  pthread_mutex_lock( &(mp->mutex));
  
  while( err == 0 && mp->command_sent == 0)
    err = pthread_cond_timedwait( &mp->cond, &mp->mutex, &timeout);
  pthread_mutex_unlock( &(mp->mutex));
  if( err != 0) {
    if( err != ETIMEDOUT) {
      lslogging_log_message( "lspmac_moveabs_wait: unexpected error from timedwait %d  tv_sec %ld   tv_nsec %ld", err, timeout.tv_sec, timeout.tv_nsec);
    }
    return 1;
  }

  //
  // wait for the motion to have started
  // This will time out if the motion ends before we can read the status back
  // hence the added complication of time stamp of the sent packet.

  err = 0;
  pthread_mutex_lock( &(mp->mutex));
  while( err == 0 && mp->motion_seen == 0)
    err = pthread_cond_timedwait( &(mp->cond), &(mp->mutex), &timeout);
  
  if( err != 0) {
    if( err != ETIMEDOUT) {
      lslogging_log_message( "lspmac_moveabs_wait: unexpected error from timedwait: %d  tv_sec %ld   tv_nsec %ld", err, timeout.tv_sec, timeout.tv_nsec);
    }
    pthread_mutex_unlock( &(mp->mutex));
    return 1;
  }

  //
  // wait for the motion that we know has started to finish
  //
  err = 0;
  while( err == 0 && mp->not_done)
    err = pthread_cond_timedwait( &(mp->cond), &(mp->mutex), &timeout);

  
  if( err != 0) {
    if( err != ETIMEDOUT) {
      lslogging_log_message( "lspmac_moveabs_wait: unexpected error from timedwait: %d  tv_sec %ld   tv_nsec %ld", err, timeout.tv_sec, timeout.tv_nsec);
    }
    pthread_mutex_unlock( &(mp->mutex));
    return 1;
  }

  //
  // if return code was not 0 then we know we shouldn't wait for not_done flag.
  // In this case the motion ended before we read the status registers
  //
  pthread_mutex_unlock( &(mp->mutex));
  return 0;
}

/** Helper funciton for the init calls
 */
void _lspmac_motor_init( lspmac_motor_t *d, char *name) {
  pthread_mutexattr_t mutex_initializer;
  // Use recursive mutexs
  //
  pthread_mutexattr_init( &mutex_initializer);
  pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);

  lspmac_nmotors++;

  pthread_mutex_init( &(d->mutex), &mutex_initializer);
  pthread_cond_init(  &(d->cond), NULL);

  d->magic               = LSPMAC_MAGIC_NUMBER;
  d->name                = strdup(name);
  d->active		 = lsredis_get_obj( "%s.active",	    d->name);
  d->active_init	 = lsredis_get_obj( "%s.active_init",	    d->name);
  d->axis		 = lsredis_get_obj( "%s.axis",	            d->name);
  d->coord_num		 = lsredis_get_obj( "%s.coord_num",         d->name);
  d->home		 = lsredis_get_obj( "%s.home",	            d->name);
  d->in_position_band    = lsredis_get_obj( "%s.in_position_band",  d->name);
  d->inactive_init	 = lsredis_get_obj( "%s.inactive_init",	    d->name);
  d->redis_fmt		 = lsredis_get_obj( "%s.format",            d->name);
  d->max_accel		 = lsredis_get_obj( "%s.max_accel",         d->name);
  d->max_speed		 = lsredis_get_obj( "%s.max_speed",         d->name);
  d->max_pos             = lsredis_get_obj( "%s.maxPosition",       d->name);
  d->min_pos             = lsredis_get_obj( "%s.minPosition",       d->name);
  d->motor_num		 = lsredis_get_obj( "%s.motor_num",         d->name);
  d->neg_limit_hit       = lsredis_get_obj( "%s.negLimitSet",       d->name);
  d->neutral_pos         = lsredis_get_obj( "%s.neutralPosition",   d->name);
  d->redis_position      = lsredis_get_obj( "%s.position",          d->name);
  d->pos_limit_hit       = lsredis_get_obj( "%s.posLimitSet",       d->name);
  d->precision           = lsredis_get_obj( "%s.precision",         d->name);
  d->printf_fmt		 = lsredis_get_obj( "%s.printf",            d->name);
  d->status_str          = lsredis_get_obj( "%s.status_str",        d->name);
  d->u2c                 = lsredis_get_obj( "%s.u2c",               d->name);
  d->unit		 = lsredis_get_obj( "%s.unit",              d->name);
  d->update_resolution   = lsredis_get_obj( "%s.update_resolution", d->name);
  d->lut                 = NULL;
  d->nlut                = 0;
  d->homing              = 0;
  d->dac_mvar            = NULL;
  d->actual_pos_cnts_p   = NULL;
  d->status1_p           = NULL;
  d->status2_p           = NULL;
  d->win                 = NULL;
  d->read                = NULL;
  d->reported_position   = INFINITY;
  d->reported_pg_position= INFINITY;

  lsevents_preregister_event( "%s queued", d->name);
  lsevents_preregister_event( "%s command accepted", d->name);

  lsredis_load_presets( d->name);
}


/** Initialize a pmac stepper or servo motor
 */
lspmac_motor_t *lspmac_motor_init(
				  lspmac_motor_t *d,				/**< [in,out] An uninitialize motor object		*/
				  int wy,					/**< [in] Curses status window row index		*/
				  int wx,					/**< [in] Curses status window column index		*/
				  int *posp,					/**< [in] Pointer to position status			*/
				  int *stat1p,					/**< [in] Pointer to 1st status word			*/
				  int *stat2p,					/**< [in] Pointer to 2nd status word			*/
				  char *wtitle,					/**< [in] Title for this motor (to display)		*/
				  char *name,					/**< [in] This motor's name		                */
				  int (*moveAbs)(lspmac_motor_t *,double),	/**< [in] Method to use to move this motor (motion program preferred)	*/
				  int (*jogAbs)(lspmac_motor_t *,double)	/**< [in] Method to use to jog this motor  (jog preferred)		*/
				  ) {

  _lspmac_motor_init( d, name);

  d->moveAbs             = moveAbs;
  d->jogAbs              = jogAbs;
  d->read                = lspmac_pmacmotor_read;
  d->actual_pos_cnts_p   = posp;
  d->status1_p           = stat1p;
  d->status2_p           = stat2p;

  d->win = newwin( LS_DISPLAY_WINDOW_HEIGHT, LS_DISPLAY_WINDOW_WIDTH, wy*LS_DISPLAY_WINDOW_HEIGHT, wx*LS_DISPLAY_WINDOW_WIDTH);

  pthread_mutex_lock( &ncurses_mutex);
  box( d->win, 0, 0);
  mvwprintw( d->win, 1, 1, "%s", wtitle);
  wnoutrefresh( d->win);
  pthread_mutex_unlock( &ncurses_mutex);

  lsevents_preregister_event( "%s Homing",       d->name);
  lsevents_preregister_event( "%s Homed",        d->name);
  lsevents_preregister_event( "%s Moving",       d->name);
  lsevents_preregister_event( "%s In Position",  d->name);
  lsevents_preregister_event( "%s Move Aborted", d->name);


  return d;
}

/** Initalize the fast shutter motor
 */
lspmac_motor_t *lspmac_fshut_init(
				  lspmac_motor_t *d		/**< [in] Our uninitialized motor object	*/
				  ) {

  _lspmac_motor_init( d, "fastShutter");

  d->moveAbs           = lspmac_moveabs_fshut_queue;
  d->jogAbs            = lspmac_moveabs_fshut_queue;
  d->read              = lspmac_shutter_read;

  return d;
}


/** Initialize binary i/o motor
 */

lspmac_motor_t *lspmac_bo_init(
				lspmac_motor_t *d,		/**< [in] Our uninitialized motor object				*/
				char *name,			/**< [in] Name of motor to coordinate with DB				*/
				char *write_fmt,		/**< [in] Format string used to generate PMAC command to move motor	*/
				int *read_ptr,			/**< [in] Pointer to byte in md2_status to find position		*/
				int read_mask			/**< [in] Bitmask to find position in *read_ptr				*/
				) {

  _lspmac_motor_init( d, name);

  d->moveAbs           = lspmac_moveabs_bo_queue;
  d->jogAbs            = lspmac_moveabs_bo_queue;
  d->read              = lspmac_bo_read;
  d->write_fmt         = strdup( write_fmt);
  d->read_ptr	       = read_ptr;
  d->read_mask         = read_mask;

  lsevents_preregister_event( "%s 1", d->name);
  lsevents_preregister_event( "%s 0", d->name);
  return d;
}


/** Initialize DAC motor
 *  Note that some motors require further initialization
 *  from a database query.
 *  For this reason this initialzation code must be run before the database
 *  queue is allowed to be processed.
 */
lspmac_motor_t *lspmac_dac_init(
				/**< [out] Returns the (almost) initialized motor object			*/
				lspmac_motor_t *d,		/**< [in,out] unitintialized motor		*/
				int *posp,			/**< [in] Location of current position		*/
				char *mvar,			/**< [in] M variable, ie, "M1200"		*/
				char *name,			/**< [in] name to coordinate with DB		*/
				int (*moveAbs)(lspmac_motor_t *,double)	/**< [in] Method to use to move this motor		*/
				) {

  _lspmac_motor_init( d, name);
  d->moveAbs           = moveAbs;
  d->jogAbs            = moveAbs;
  d->read              = lspmac_dac_read;
  d->actual_pos_cnts_p = posp;
  d->dac_mvar          = strdup(mvar);

  return d;
}

/** Dummy routine to read a soft motor
 */
void lspmac_soft_motor_read( lspmac_motor_t *p) {

}


lspmac_motor_t *lspmac_soft_motor_init( lspmac_motor_t *d, char *name, int (*moveAbs)(lspmac_motor_t *, double)) {

  _lspmac_motor_init( d, name);

  d->moveAbs      = moveAbs;
  d->jogAbs       = moveAbs;
  d->read         = lspmac_soft_motor_read;
  d->actual_pos_cnts_p = calloc( sizeof(int), 1);
  *d->actual_pos_cnts_p = 0;

  return d;
}


/** Initialize binary input
 */
lspmac_bi_t *lspmac_bi_init( lspmac_bi_t *d, int *ptr, int mask, char *onEvent, char *offEvent) {
  lspmac_nbis++;
  pthread_mutex_init( &(d->mutex), NULL);
  d->ptr            = ptr;
  d->mask           = mask;
  d->changeEventOn  = strdup( onEvent);
  d->changeEventOff = strdup( offEvent);
  d->first_time     = 1;

  lsevents_preregister_event( "%s", d->changeEventOn);
  lsevents_preregister_event( "%s", d->changeEventOff);

  return d;
}

/** reset and resetart
 */
void lspmac_full_card_reset_cb( char *event) {
  lspmac_running = 0;
  pthread_join( pmac_thread, NULL);
  pthread_mutex_lock( &pmac_queue_mutex);
  
  ethCmdOn    = 0;
  ethCmdOff   = 0;
  ethCmdReply = 0;

  lspmac_running = 1;
  ls_pmac_state = LS_PMAC_STATE_DETACHED;

  pthread_mutex_unlock( &pmac_queue_mutex);

  lspmac_init( 0, 0);
  lspmac_run();
}


/** Initialize this module
 */
void lspmac_init(
		 int ivarsflag,		/**< [in]  Set global flag to harvest i variables			*/
		 int mvarsflag		/**< [in]  Set global flag to harvest m variables			*/
		 ) {
  static int first_time = 1;
  int i;
  int err;
  ENTRY entry_in, *entry_outp;
  md2_status_t *p;
  pthread_mutexattr_t mutex_initializer;

  if( first_time) {
    // Set our global harvest flags
    getivars = ivarsflag;
    getmvars = mvarsflag;

    // Use recursive mutexs
    //
    pthread_mutexattr_init( &mutex_initializer);
    pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);

    // All important status mutex
    pthread_mutex_init( &md2_status_mutex, &mutex_initializer);

    //
    // Get the MD2 initialization strings
    //
    //  lspmac_md2_init = lsredis_get_obj( "md2_pmac.init");  // hard coded now.

    //
    // Initialize the motor objects
    //

    p = &md2_status;

    omega  = lspmac_motor_init( &(lspmac_motors[ 0]), 0, 0, &p->omega_act_pos,     &p->omega_status_1,     &p->omega_status_2,     "Omega   #1 &1 X", "omega",       lspmac_moveabs_queue, lspmac_jogabs_queue);
    alignx = lspmac_motor_init( &(lspmac_motors[ 1]), 0, 1, &p->alignx_act_pos,    &p->alignx_status_1,    &p->alignx_status_2,    "Align X #2 &3 X", "align.x",     lspmac_moveabs_queue, lspmac_jogabs_queue);
    aligny = lspmac_motor_init( &(lspmac_motors[ 2]), 0, 2, &p->aligny_act_pos,    &p->aligny_status_1,    &p->aligny_status_2,    "Align Y #3 &3 Y", "align.y",     lspmac_moveabs_queue, lspmac_jogabs_queue);
    alignz = lspmac_motor_init( &(lspmac_motors[ 3]), 0, 3, &p->alignz_act_pos,    &p->alignz_status_1,    &p->alignz_status_2,    "Align Z #4 &3 Z", "align.z",     lspmac_moveabs_queue, lspmac_jogabs_queue);
    anal   = lspmac_motor_init( &(lspmac_motors[ 4]), 0, 4, &p->analyzer_act_pos,  &p->analyzer_status_1,  &p->analyzer_status_2,  "Anal    #5",      "lightPolar",  lspmac_moveabs_queue, lspmac_jogabs_queue);
    zoom   = lspmac_motor_init( &(lspmac_motors[ 5]), 1, 0, &p->zoom_act_pos,      &p->zoom_status_1,      &p->zoom_status_2,      "Zoom    #6 &4 Z", "cam.zoom",    lspmac_movezoom_queue, lspmac_movezoom_queue);
    apery  = lspmac_motor_init( &(lspmac_motors[ 6]), 1, 1, &p->aperturey_act_pos, &p->aperturey_status_1, &p->aperturey_status_2, "Aper Y  #7 &5 Y", "appy",        lspmac_moveabs_queue, lspmac_jogabs_queue);
    aperz  = lspmac_motor_init( &(lspmac_motors[ 7]), 1, 2, &p->aperturez_act_pos, &p->aperturez_status_1, &p->aperturez_status_2, "Aper Z  #8 &5 Z", "appz",        lspmac_moveabs_queue, lspmac_jogabs_queue);
    capy   = lspmac_motor_init( &(lspmac_motors[ 8]), 1, 3, &p->capy_act_pos,      &p->capy_status_1,      &p->capy_status_2,      "Cap Y   #9 &5 U", "capy",        lspmac_moveabs_queue, lspmac_jogabs_queue);
    capz   = lspmac_motor_init( &(lspmac_motors[ 9]), 1, 4, &p->capz_act_pos,      &p->capz_status_1,      &p->capz_status_2,      "Cap Z  #10 &5 V", "capz",        lspmac_moveabs_queue, lspmac_jogabs_queue);
    scint  = lspmac_motor_init( &(lspmac_motors[10]), 2, 0, &p->scint_act_pos,     &p->scint_status_1,     &p->scint_status_2,     "Scin Z #11 &5 W", "scint",       lspmac_moveabs_queue, lspmac_jogabs_queue);
    cenx   = lspmac_motor_init( &(lspmac_motors[11]), 2, 1, &p->centerx_act_pos,   &p->centerx_status_1,   &p->centerx_status_2,   "Cen X  #17 &2 X", "centering.x", lspmac_moveabs_queue, lspmac_jogabs_queue);
    ceny   = lspmac_motor_init( &(lspmac_motors[12]), 2, 2, &p->centery_act_pos,   &p->centery_status_1,   &p->centery_status_2,   "Cen Y  #18 &2 Y", "centering.y", lspmac_moveabs_queue, lspmac_jogabs_queue);
    kappa  = lspmac_motor_init( &(lspmac_motors[13]), 2, 3, &p->kappa_act_pos,     &p->kappa_status_1,     &p->kappa_status_2,     "Kappa  #19 &7 X", "kappa",       lspmac_moveabs_queue, lspmac_jogabs_queue);
    phi    = lspmac_motor_init( &(lspmac_motors[14]), 2, 4, &p->phi_act_pos,       &p->phi_status_1,       &p->phi_status_2,       "Phi    #20 &7 Y", "phi",         lspmac_moveabs_queue, lspmac_jogabs_queue);

    fshut  = lspmac_fshut_init( &(lspmac_motors[15]));
    flight = lspmac_dac_init( &(lspmac_motors[16]), &p->front_dac,   "M1200", "frontLight.intensity", lspmac_movedac_queue);
    blight = lspmac_dac_init( &(lspmac_motors[17]), &p->back_dac,    "M1201", "backLight.intensity",  lspmac_movedac_queue);
    fscint = lspmac_dac_init( &(lspmac_motors[18]), &p->scint_piezo, "M1203", "scint.focus",          lspmac_movedac_queue);

    smart_mag_oo  = lspmac_bo_init( &(lspmac_motors[19]), "smartMagnet","M1100=%d", &(md2_status.acc11c_5), 0x01);
    blight_ud     = lspmac_bo_init( &(lspmac_motors[20]), "backLight",  "M1101=%d", &(md2_status.acc11c_5), 0x02);
    cryo          = lspmac_bo_init( &(lspmac_motors[21]), "cryo",       "M1102=%d", &(md2_status.acc11c_5), 0x04);
    dryer         = lspmac_bo_init( &(lspmac_motors[22]), "dryer",      "M1103=%d", &(md2_status.acc11c_5), 0x08);
    fluo          = lspmac_bo_init( &(lspmac_motors[23]), "fluo",       "M1104=%d", &(md2_status.acc11c_5), 0x10);
    flight_oo     = lspmac_soft_motor_init( &(lspmac_motors[24]), "frontLight",        lspmac_moveabs_frontlight_oo_queue);
    blight_f      = lspmac_soft_motor_init( &(lspmac_motors[25]), "backLight.factor",  lspmac_moveabs_blight_factor_queue);
    flight_f      = lspmac_soft_motor_init( &(lspmac_motors[26]), "frontLight.factor", lspmac_moveabs_flight_factor_queue);

    lp_air          = lspmac_bi_init( &(lspmac_bis[ 0]), &(md2_status.acc11c_1),  0x01, "Low Pressure Air OK",  "Low Pressure Air Failed");
    hp_air          = lspmac_bi_init( &(lspmac_bis[ 1]), &(md2_status.acc11c_1),  0x02, "High Pressure Air OK", "High Pressure Air Failed");
    cryo_switch     = lspmac_bi_init( &(lspmac_bis[ 2]), &(md2_status.acc11c_1),  0x04, "CryoSwitchChanged",    "CryoSwitchChanged");
    blight_down     = lspmac_bi_init( &(lspmac_bis[ 3]), &(md2_status.acc11c_1),  0x08, "Backlight Down",       "Backlight Not Down");
    blight_up       = lspmac_bi_init( &(lspmac_bis[ 4]), &(md2_status.acc11c_1),  0x10, "Backlight Up",         "Backlight Not Up");
    cryo_back       = lspmac_bi_init( &(lspmac_bis[ 5]), &(md2_status.acc11c_1),  0x40, "Cryo Back",            "Cryo Not Back");
    fluor_back	  = lspmac_bi_init( &(lspmac_bis[ 6]), &(md2_status.acc11c_2),  0x01, "Fluor. Det. Parked",   "Fluor. Det. Not Parked");
    sample_detected = lspmac_bi_init( &(lspmac_bis[ 7]), &(md2_status.acc11c_2),  0x02, "SamplePresent",        "SampleAbsent");
    etel_ready      = lspmac_bi_init( &(lspmac_bis[ 8]), &(md2_status.acc11c_2),  0x20, "ETEL Ready",           "ETEL Not Ready");
    etel_on         = lspmac_bi_init( &(lspmac_bis[ 9]), &(md2_status.acc11c_2),  0x40, "ETEL On",              "ETEL Off");
    etel_init_ok    = lspmac_bi_init( &(lspmac_bis[10]), &(md2_status.acc11c_2),  0x80, "ETEL Init OK",         "ETEL Init Not OK");
    minikappa_ok    = lspmac_bi_init( &(lspmac_bis[11]), &(md2_status.acc11c_3),  0x01, "Minikappa OK",         "Minikappa Not OK");
    smart_mag_on    = lspmac_bi_init( &(lspmac_bis[12]), &(md2_status.acc11c_3),  0x04, "Smart Magnet On",      "Smart Magnet Not On");
    arm_parked      = lspmac_bi_init( &(lspmac_bis[13]), &(md2_status.acc11c_3),  0x08, "Arm Parked",           "Arm Not Parked");
    smart_mag_err   = lspmac_bi_init( &(lspmac_bis[14]), &(md2_status.acc11c_3),  0x10, "Smart Magnet Error",   "Smart Magnet OK");
    shutter_open    = lspmac_bi_init( &(lspmac_bis[15]), &(md2_status.acc11c_3), 0x100, "Shutter Open",         "Shutter Not Open");
    smart_mag_off   = lspmac_bi_init( &(lspmac_bis[16]), &(md2_status.acc11c_5),  0x01, "Smart Magnet Off",     "Smart Magnet Not Off");
  


    // Set up hash table
    //
    err = hcreate_r( LSPMAC_MAX_MOTORS * 2, &motors_ht);
    if( err == 0) {
      lslogging_log_message( "lspmac_init: hcreate_r failed: '%s'", strerror( errno));
      exit( -1);
    }
    for( i=0; i<lspmac_nmotors; i++) {
      entry_in.key   = lspmac_motors[i].name;
      entry_in.data  = &(lspmac_motors[i]);
      err = hsearch_r( entry_in, ENTER, &entry_outp, &motors_ht);
      if( err == 0) {
	lslogging_log_message( "lspmac_init: hsearch_r failed for motor %s: '%s'", lspmac_motors[i].name, strerror( errno));
	exit( -1);
      }
    }



    //
    // Initialize several commands that get called, perhaps, alot
    //
    rr_cmd.RequestType = VR_UPLOAD;
    rr_cmd.Request     = VR_PMAC_READREADY;
    rr_cmd.wValue      = 0;
    rr_cmd.wIndex      = 0;
    rr_cmd.wLength     = htons(2);
    memset( rr_cmd.bData, 0, sizeof(rr_cmd.bData));

    gb_cmd.RequestType = VR_UPLOAD;
    gb_cmd.Request     = VR_PMAC_GETBUFFER;
    gb_cmd.wValue      = 0;
    gb_cmd.wIndex      = 0;
    gb_cmd.wLength     = htons(1400);
    memset( gb_cmd.bData, 0, sizeof(gb_cmd.bData));

    cr_cmd.RequestType = VR_UPLOAD;
    cr_cmd.Request     = VR_CTRL_RESPONSE;
    cr_cmd.wValue      = 0;
    cr_cmd.wIndex      = 0;
    cr_cmd.wLength     = htons(1400);
    memset( cr_cmd.bData, 0, sizeof(cr_cmd.bData));

    //
    // Initialize some mutexs and conditions
    //

    pthread_mutex_init( &pmac_queue_mutex, &mutex_initializer);
    pthread_cond_init(  &pmac_queue_cond, NULL);

    lspmac_shutter_state = 0;				// assume the shutter is now closed: not a big deal if we are wrong
    pthread_mutex_init( &lspmac_shutter_mutex, &mutex_initializer);
    pthread_cond_init(  &lspmac_shutter_cond, NULL);
    pmacfd.fd = -1;

    pthread_mutex_init( &lspmac_moving_mutex, &mutex_initializer);
    pthread_cond_init(  &lspmac_moving_cond, NULL);

    pthread_mutex_init( &lspmac_ascii_mutex, &mutex_initializer);

    pthread_mutex_init( &lspmac_ascii_buffers_mutex, &mutex_initializer);

    lsevents_preregister_event( "omega crossed zero");
    lsevents_preregister_event( "Move Aborted");
    lsevents_preregister_event( "Combined Move Aborted");
    lsevents_preregister_event( "Abort Request queued");
    lsevents_preregister_event( "Abort Request accepted");
    lsevents_preregister_event( "Reset queued");
    lsevents_preregister_event( "Reset command accepted");
  

    for( i=1; i<=16; i++) {
      lsevents_preregister_event( "Coordsys %d Stopped", i);
    }
    first_time = 0;
  }
  //
  // clear the ascii communications buffers
  //
  {
    uint32_t cc;
    cc = 0;
    lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0e9e, 0, 4, (char *)&cc, NULL, 1, NULL);

    cc = 0x18;
    lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0e9e, 0, 4, (char *)&cc, NULL, 1, NULL);
  }

  lspmac_SockSendDPline( NULL, "I5=0");
  lspmac_SockSendDPline( NULL, "ENABLE PLCC 0,2");
  lspmac_SockSendDPline( NULL, "DISABLE PLCC 1");
  lspmac_SockSendDPline( NULL, "I5=3");


}


void lspmac_cryoSwitchChanged_cb( char *event) {
  int pos;

  pthread_mutex_lock( &(cryo->mutex));
  pos = cryo->position;
  pthread_mutex_unlock( &(cryo->mutex));

  cryo->moveAbs( cryo, pos ? 0.0 : 1.0);
}

/** Maybe start drying off the scintilator
 *  \param event required by protocol
 */
void lspmac_scint_maybe_turn_on_dryer_cb( char *event) {
  static int trigger = 0;
  double pos;
  double cover;
  int err;

  pthread_mutex_lock( &(scint->mutex));
  pos = scint->position;
  pthread_mutex_unlock( &(scint->mutex));

  if( pos > 20.0) {
    trigger = 1;
    return;
  }

  if( trigger == 0) {
    return;
  }

  err = lsredis_find_preset( scint->name, "Cover", &cover);

  lslogging_log_message( "lspmac_scint_inPosition_cb: pos %f, cover %f, diff %f, err %d", pos, cover, fabs( pos-cover), err);

  if( err == 0)
    return;

  if( fabs( pos - cover) <= 0.1) {
    dryer->moveAbs( dryer, 1.0);
    lslogging_log_message( "lspmac_scint_inPosition_cb: Starting dryer");
    lstimer_set_timer( "scintDried", 1, 120, 0);
    trigger = 0;
  }
}

/** Maybe stop drying off the scintilator
 *  \param event required by protocol
 */
void lspmac_scint_maybe_turn_off_dryer_cb( char *event) {
  double pos;

  //
  // See if the dryer is on
  //
  pos = lspmac_getPosition( dryer);

  if( pos == 0.0)
    return;

  dryer->moveAbs( dryer, 0.0);
  
  lstimer_unset_timer( "scintDried");

}

/** Turn on the backlight whenever it goes up
 *  \param event Name of the event that called us
 */
void lspmac_backLight_up_cb( char *event) {
  blight->moveAbs( blight, (int)(lspmac_getPosition( zoom)));
}

/** Turn off the backlight whenever it goes down
 *  \param event Name of the event that called us
 */
void lspmac_backLight_down_cb( char *event) {
  blight->moveAbs( blight, 0.0);
}

/** Set the backlight intensity whenever the zoom is changed (and the backlight is up)
 *  \param event Name of the event that calledus
 */
void lspmac_light_zoom_cb( char *event) {
  int z;

  pthread_mutex_lock( &zoom->mutex);
  z = zoom->requested_position;
  pthread_mutex_unlock( &zoom->mutex);

  lslogging_log_message( "lspmac_light_zoom_cb: zoom = %d", z);

  if( lspmac_getPosition( flight_oo) != 0.0) {
    flight->moveAbs( flight, (double)z);
    } else {
      flight->moveAbs( flight, 0.0);
    }
  if( lspmac_getPosition( blight_ud) != 0.0) {
    blight->moveAbs( blight, (double)z);
  } else {
    blight->moveAbs( blight, 0.0);
  }
}

/** prepare to exit program in a couple of seconds
 */
void lspmac_quitting_cb( char *event) {
  double move_time;
  int mmask;

  pgpmac_request_stay_of_execution( 1);
  fshut->moveAbs( fshut, 0.0);
  dryer->moveAbs( dryer, 0.0);
  
  lspmac_est_move_time( &move_time, &mmask,
			aperz,  1, "Cover", 0.0,
			capz,   1, "Cover", 0.0,
			scint,  1, "Cover", 0.0,
			blight, 1,  NULL,   0.0,
			flight, 1,  NULL,   0.0,
			NULL);
  
  pgpmac_request_stay_of_execution( ((int)move_time) + 2);
  
}

/** Perhaps we need to move the sample out of the way
 */
void lspmac_scint_maybe_move_sample_cb( char *event) {
  static int trigger = 1;
  double scint_target;
  int err;
  double move_time;
  int mmask;

  pthread_mutex_lock( &scint->mutex);
  scint_target = scint->requested_position;
  pthread_mutex_unlock( &scint->mutex);
  
  // This should be pretty conservative since the out position is around 80
  //
  if( scint_target > 10.0) {
    if( trigger) {
      mmask = 0;
      err = lspmac_est_move_time( &move_time, &mmask,
				  alignx, 0, "Back", -2.0,
				  aligny, 0, "Back",  1.0,
				  alignz, 0, "Back",  1.0,
				  NULL);
      if( err) {
	lspmac_abort();
	lsevents_send_event( "Move Aborted");
	lslogging_log_message( "lspmac_scint_maybe_move_sample_cb: Failed move request, aborting motion to keep scint from hitting sample");
      }    
      trigger = 0;
    }
  } else {
    trigger = 1;
  }
}

/** Perhaps we need to return the sample to the beam
 */
void lspmac_scint_maybe_return_sample_cb( char *event) {
  static int trigger = 1;
  double scint_target;
  double move_time;
  int mmask;

  pthread_mutex_lock( &scint->mutex);
  scint_target = scint->requested_position;
  pthread_mutex_unlock( &scint->mutex);
  
  // This should be pretty conservative since the out position is around 80
  //
  if( scint_target < 10.0) {
    if( trigger) {
      mmask = 0;
      lspmac_est_move_time( &move_time, &mmask,
			    alignx, 0, "Beam",  0.0,
			    aligny, 0, "Beam",  0.0,
			    alignz, 0, "Beam",  0.0,
			    NULL);
      trigger = 0;
    }
  } else {
    trigger = 1;
  }
}


/** Turn off the dryer
 *  \param event required by protocol
 */
void lspmac_scint_dried_cb( char *event) {
  lslogging_log_message( "lspmac_scint_dried_cb: Stopping dryer");
  dryer->moveAbs( dryer, 0.0);
}


/** Set up lookup table for zoom
 */
void lspmac_zoom_lut_setup() {
  int i;
  lsredis_obj_t *p;
  double neutral_pos;

  neutral_pos = lsredis_getd( zoom->neutral_pos);

  pthread_mutex_lock( &zoom->mutex);

  zoom->nlut = 10;
  zoom->lut = calloc( 2 * zoom->nlut, sizeof( double));
  if( zoom->lut == NULL) {
    lslogging_log_message( "lspmac_zoom_lut_setup: out of memory");
    exit( -1);
  }

  for( i=0; i < zoom->nlut; i++) {
    p = lsredis_get_obj( "cam.zoom.%d.MotorPosition", i+1);
    if( p==NULL || strlen( lsredis_getstr(p)) == 0) {
      free( zoom->lut);
      zoom->lut  = NULL;
      zoom->nlut = 0;
      pthread_mutex_unlock( &zoom->mutex);
      lslogging_log_message( "lspmac_zoom_lut_setup: cannot find MotorPosition element for cam.zoom level %d", i+1);
      return;
    }
    zoom->lut[2*i]   = i+1;
    zoom->lut[2*i+1] = lsredis_getd( p) + neutral_pos;
  }
  pthread_mutex_unlock( &zoom->mutex);
}

/** Set up lookup table for flight
 */
void lspmac_flight_lut_setup() {
  int i;
  lsredis_obj_t *p;

  pthread_mutex_lock( &flight->mutex);

  flight->nlut = 11;
  flight->lut = calloc( 2 * flight->nlut, sizeof( double));
  if( flight->lut == NULL) {
    lslogging_log_message( "lspmac_flight_lut_setup: out of memory");
    exit( -1);
  }

  flight->lut[0] = 0;
  flight->lut[1] = 0;
  for( i=1; i < flight->nlut; i++) {
    p = lsredis_get_obj( "cam.zoom.%d.FrontLightIntensity", i);
    if( p==NULL || strlen( lsredis_getstr(p)) == 0) {
      free( flight->lut);
      flight->lut  = NULL;
      flight->nlut = 0;
      pthread_mutex_unlock( &flight->mutex);
      lslogging_log_message( "lspmac_flight_lut_setup: cannot find MotorPosition element for cam.flight level %d", i);
      return;
    }
    flight->lut[2*i]   = i;
    flight->lut[2*i+1] = 32767.0 * lsredis_getd( p) / 100.0;
  }
  pthread_mutex_unlock( &flight->mutex);
}

/** Set up lookup table for blight
 */
void lspmac_blight_lut_setup() {
  int i;
  lsredis_obj_t *p;

  pthread_mutex_lock( &blight->mutex);

  blight->nlut = 11;
  blight->lut = calloc( 2 * blight->nlut, sizeof( double));
  if( blight->lut == NULL) {
    lslogging_log_message( "lspmac_blight_lut_setup: out of memory");
    exit( -1);
  }

  blight->lut[0] = 0;
  blight->lut[1] = 0;

  for( i=1; i<blight->nlut; i++) {
    p = lsredis_get_obj( "cam.zoom.%d.LightIntensity", i);
    if( p==NULL || strlen( lsredis_getstr(p)) == 0) {
      free( blight->lut);
      blight->lut = NULL;
      blight->nlut = 0;
      pthread_mutex_unlock( &blight->mutex);
      lslogging_log_message( "lspmac_blight_lut_setup: cannot find MotorPosition element for cam.blight level %d", i);
      return;
    }
    blight->lut[2*i]   = i;
    blight->lut[2*i+1] = 20000.0 * lsredis_getd( p) / 100.0;
  }
  for( i=0; i<blight->nlut; i++) {
    lslogging_log_message( "lspmac_blight_lut_setup:  i: %d  x: %f  y: %f  y(lut): %f  x(rlut): %f",
			   i, blight->lut[2*i], blight->lut[2*i+1],
			   lspmac_lut( blight->nlut, blight->lut, blight->lut[2*i]),
			   lspmac_rlut( blight->nlut, blight->lut, blight->lut[2*i+1])
			   );
  }
  pthread_mutex_unlock( &blight->mutex);
}

/** Set up lookup table for fscint
 */
void lspmac_fscint_lut_setup() {
  int i;

  pthread_mutex_lock( &fscint->mutex);

  fscint->nlut = 101;
  fscint->lut = calloc( 2 * fscint->nlut, sizeof( double));
  if( fscint->lut == NULL) {
    lslogging_log_message( "lspmac_fscint_lut_setup: out of memory");
    exit( -1);
  }

  for( i=0; i<fscint->nlut; i++) {
    fscint->lut[2*i] = i;
    fscint->lut[2*i+1] = 320.0 * i;
  }
  pthread_mutex_unlock( &fscint->mutex);
}

lspmac_motor_t *lspmac_find_motor_by_name( char *name) {
  lspmac_motor_t *rtn;
  ENTRY entry_in, *entry_outp;
  int err;
  
  entry_in.key  = name;
  entry_in.data = NULL;
  err = hsearch_r( entry_in, FIND, &entry_outp, &motors_ht);
  if( err == 0) {
    lslogging_log_message( "lspmac_find_motor_by_name: hsearch_r failed for motor '%s': %s", name, strerror( errno));
    return NULL;
  }    
  rtn = entry_outp->data;

  return rtn;
}

void lspmac_command_done_cb( char *event) {
  int i;
  char s[32];
  lspmac_motor_t *mp;

  s[0] = 0;
  for( i=0; i<sizeof(s)-1 && event[i]; i++) {
    s[i] = 0;
    if( event[i] == ' ')
      break;
    s[i] = event[i];
  }

  mp = lspmac_find_motor_by_name( s);

  if( mp == NULL)
    return;

  pthread_mutex_lock( &(mp->mutex));

  mp->command_sent = 1;

  pthread_cond_signal( &(mp->cond));
  pthread_mutex_unlock( &(mp->mutex));

  return;
}


/** Start up the lspmac thread
 */
void lspmac_run() {
  static int first_time = 1;
  char **inits;
  lspmac_motor_t *mp;
  char evts[64];
  int i;
  int active;
  int motor_num;

  pthread_create( &pmac_thread, NULL, lspmac_worker, NULL);

  if( first_time) {
    first_time = 0;
    lsevents_add_listener( "^CryoSwitchChanged$",        lspmac_cryoSwitchChanged_cb);
    lsevents_add_listener( "^scint In Position$",        lspmac_scint_maybe_turn_on_dryer_cb);
    lsevents_add_listener( "^scint Moving$",             lspmac_scint_maybe_turn_off_dryer_cb);
    lsevents_add_listener( "^scintDried$",               lspmac_scint_dried_cb);
    lsevents_add_listener( "^backLight 1$" ,	       lspmac_backLight_up_cb);
    lsevents_add_listener( "^backLight 0$" ,	       lspmac_backLight_down_cb);
    lsevents_add_listener( "^cam.zoom Moving$",          lspmac_light_zoom_cb);
    //    lsevents_add_listener( "^Quitting Program$",         lspmac_quitting_cb);
    lsevents_add_listener( "^Control-[BCFGV] accepted$", lspmac_request_control_response_cb);
    lsevents_add_listener( "^Full Card Reset$",          lspmac_full_card_reset_cb);

    if( pgpmac_use_autoscint) {
      lsevents_add_listener( "^scint In Position$",        lspmac_scint_maybe_return_sample_cb);
      lsevents_add_listener( "^scint Moving$",             lspmac_scint_maybe_move_sample_cb);
    }


    for( i=0; i<lspmac_nmotors; i++) {
      snprintf( evts, sizeof( evts)-1, "^%s command accepted$", lspmac_motors[i].name);
      evts[sizeof(evts)-1] = 0;
      lsevents_add_listener( evts, lspmac_command_done_cb);
    }



    lspmac_zoom_lut_setup();
    lspmac_flight_lut_setup();
    lspmac_blight_lut_setup();
    lspmac_fscint_lut_setup();
  }

  //
  // Clear the command interfaces
  //
  // lspmac_SockSendControlCharPrint( "Control-X", '\x18'); // why does this kill the initialzation?

  {
    uint32_t cc;
    cc = 0;
    lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0e9e, 0, 4, (char *)&cc, NULL, 1, NULL);

    cc = 0x18;
    lspmac_send_command( VR_UPLOAD, VR_PMAC_SETMEM, 0x0e9e, 0, 4, (char *)&cc, NULL, 1, NULL);
  }
  //
  // Initialize the MD2 pmac (ie, turn on the right plcc's etc)
  //
  /*
  for( inits = lsredis_get_string_array(lspmac_md2_init); *inits != NULL; inits++) {
    lspmac_SockSendDPline( NULL, *inits);
  }
  */

  //
  // Initialize the pmac's support for each motor
  // (ie, set the various flag for when a motor is active or not)
  //
  for( i=0; i<lspmac_nmotors; i++) {
    mp        = &(lspmac_motors[i]);
    active    = lsredis_getb( mp->active);
    motor_num = lsredis_getl( mp->motor_num);

    if( motor_num >= 1 && motor_num <= 32) {
      
      //
      // Set the PMAC to be consistant with redis
      //
      lspmac_SockSendDPline( NULL, "I%d16=%f I%d17=%f I%d28=%d", motor_num, lsredis_getd( mp->max_speed), motor_num, lsredis_getd( mp->max_accel), motor_num, lsredis_getl( mp->in_position_band));
    }    

    // if there is a problem with "active" then don't do anything
    // On the other hand, various combinations of yes/no true/fals 1/0 should work
    //
    switch( active) {
    case 1:
      inits = lsredis_get_string_array( mp->active_init);
      break;

    case 0:
      inits = lsredis_get_string_array( mp->inactive_init);
      break;

    default:
      lslogging_log_message( "lspmac_run: motor %s is neither active nor inactive (!?)", mp->name);
      inits = NULL;
    }
    if( inits != NULL) {
      while( *inits != NULL) {
	lspmac_SockSendDPline( NULL, *inits);
	inits++;
      }
    }
  }
}
