#include "pgpmac.h"
/*! \file lspmac.c
 *  \brief Routines concerned with communication with PMAC.
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved

  This is a state machine (surprise!)
  Lacking is support for writingbuffer, control writing and reading, as well as double buffered memory
  It looks like several different methods of managing PMAC communications are possible.  Here is set up a queue
  of outgoing commands and deal completely with the result before sending the next.  A full handshake of acknowledgements
  and "readready" is expected.


<table>

<tr><th> State  </th><th>Description									</th></tr>
<tr><td>  -1    </td><td>  Reset the connection								</td></tr>
<tr><td>   0    </td><td>  Detached: need to connect to tcp port					</td></tr>
<tr><td>   1	</td><td>  Idle (waiting for a command to send to the pmac)				</td></tr>
<tr><td>   2	</td><td>  Send command									</td></tr>
<tr><td>   3	</td><td>  Waiting for command acknowledgement (no further response expected)		</td></tr>
<tr><td>   4	</td><td>  Waiting for control character acknowledgement (further response expected)	</td></tr>
<tr><td>   5	</td><td>  Waiting for command acknowledgement (further response expected)              </td></tr>
<tr><td>   6	</td><td>  Waiting for get memory response						</td></tr>
<tr><td>   7	</td><td>  Send controlresponse								</td></tr>
<tr><td>   8	</td><td>  Send readready								</td></tr>
<tr><td>   9	</td><td>  Waiting for acknowledgement of "readready"					</td></tr>
<tr><td>  10	</td><td>  Send readbuffer								</td></tr>
<tr><td>  11	</td><td>  Waiting for control response							</td></tr>
<tr><td>  12	</td><td>  Waiting for readbuffer response						</td></tr>

</table>
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

static lsredis_obj_t *lspmac_md2_init;

int lspmac_shutter_state;			//!< State of the shutter, used to detect changes
int lspmac_shutter_has_opened;			//!< Indicates that the shutter had opened, perhaps briefly even if the state did not change
pthread_mutex_t lspmac_shutter_mutex;		//!< Coordinates threads reading shutter status
pthread_cond_t  lspmac_shutter_cond;		//!< Allows waiting for the shutter status to change
pthread_mutex_t lspmac_moving_mutex;		//!< Coordinate moving motors between threads
pthread_cond_t  lspmac_moving_cond;		//!< Wait for motor(s) to finish moving condition
int lspmac_moving_flags;			//!< Flag used to implement motor moving condition

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

lspmac_bi_t lspmac_bis[16];			//!< array of binary inputs
int lspmac_nbis = 0;				//!< number of active binary inputs

lspmac_motor_t lspmac_motors[48];		//!< All our motors
int lspmac_nmotors = 0;				//!< The number of motors we manage
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

lspmac_motor_t *blight_ud;			//!< Back light Up/Down actuator
lspmac_motor_t *flight_oo;			//!< Turn front light on/off
lspmac_motor_t *blight_f;			//!< Back light scale factor
lspmac_motor_t *flight_f;			//!< Front light scale factor
lspmac_motor_t *cryo;				//!< Move the cryostream towards or away from the crystal
lspmac_motor_t *dryer;				//!< blow air on the scintilator to dry it off
lspmac_motor_t *fluo;				//!< Move the fluorescence detector in/out

lspmac_bi_t    *cryo_switch;			//!< that little toggle switch for the cryo

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


static int linesReceived=0;			//!< current number of lines received
static unsigned char dbmem[64*1024];		//!< double buffered memory
static int dbmemIn = 0;				//!< next location

//! Minimum time between commands to the pmac
//
#define PMAC_MIN_CMD_TIME 20000.0
static struct timeval pmac_time_sent, now;	//!< used to ensure we do not send commands to the pmac too often.  Only needed for non-DB commands.

//! Size of the PMAC command queue.
#define PMAC_CMD_QUEUE_LENGTH 2048
static pmac_cmd_t rr_cmd, gb_cmd, cr_cmd;		//!< commands to send out "readready", "getbuffer", controlresponse (initialized in main)
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
  "ERR019: Illegal position-chage command while moves stored in CCBUFFER"
};


//! The block of memory retrieved in a status request.

//
// DPRAM is from $60000 to ????
//
// We can quickly read 1400 bytes = 350 32-bit registers
// so reading $60000 to 60015D is not too expensive
//
// Gather data starts at $600450 (per turbo pmac user manual)
//
// MD2 seems to use $60060 to
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

  int moving_flags;		// 0x11C		$60145
} md2_status_t;

static md2_status_t md2_status;		//!< Buffer for MD2 Status
pthread_mutex_t md2_status_mutex;	//!< Synchronize reading/writting status buffer


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

    if( lut[1] < lut[2*nlut-1])
      up = 1;
    else
      up = 0;

    for( i=0; i < 2*nlut; i += 2) {
      x1 = lut[i];
      y1 = lut[i+1];
      if( i < 2*nlut - 2) {
	x2 = lut[i+2];
	y2 = lut[i+3];
      }
      if( i==0 && ( up ? y1 > y : y1 < y)) {
	x = x1;
	foundone = 1;
	break;
      }
      if( y1 == y) {
	x = x1;
	foundone = 1;
	break;
      }
      if( (i < 2*nlut-2) && (up ? y < y2 : y > y2)) {
	m = (x2 - x1) / (y2 - y1);
	x = m * (y - y1) + x1;
	foundone = 1;
	break;
      }
    }
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
	      unsigned char *s	/**< [in] Data to dump			*/
	      ) {

  int i;	// row counter
  int j;	// column counter

  pthread_mutex_lock( &ncurses_mutex);

  for( i=0; n > 0; i++) {
    for( j=0; j<16 && n > 0; j++) {
      if( j==8)
	wprintw( term_output, "  ");
      wprintw( term_output, " %02x", *(s + 16*i + j));
      n--;
    }
    wprintw( term_output, "\n");
  }
  wprintw( term_output, "\n");

  pthread_mutex_unlock( &ncurses_mutex);
}

/** Replace \\r with \\n in null terminated string and print result to terminal.
 * Needed to turn PMAC messages into something printable.
 */

void cleanstr(
	      char *s	/**< [in] String to print to terminal.	*/
	      ) {
  int i;

  pthread_mutex_lock( &ncurses_mutex);

  for( i=0; i<strlen( s); i++) {
    if( s[i] == '\r')
      wprintw( term_output, "\n");
    else
      wprintw( term_output, "%c", s[i]);
  }

  pthread_mutex_unlock( &ncurses_mutex);
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
				      unsigned char *data,	/**< [in] Data array (or NULL)				*/
				      void (*responseCB)(pmac_cmd_queue_t *, int, unsigned char *),
				      /**< [in] Function to call when a response is read from the PMAC			*/
				      int no_reply		/**< [in] Flag, non-zero means no reply is expected	*/
				      ) {
  static pmac_cmd_queue_t cmd;

  cmd.pcmd.RequestType = rqType;
  cmd.pcmd.Request     = rq;
  cmd.pcmd.wValue      = htons(wValue);
  cmd.pcmd.wIndex      = htons(wIndex);
  cmd.pcmd.wLength     = htons(wLength);
  cmd.onResponse       = responseCB;
  cmd.no_reply	       = no_reply;

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
  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_FLUSH, 0, 0, 0, NULL, NULL, 1);
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
		  unsigned char *buff	/**< [in] Buffer returned by PMAC perhaps containing a NULL terminated message.	*/
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

      pthread_mutex_lock( &ncurses_mutex);
      wprintw( term_output, "\n%s\n", pmac_error_strs[err]);
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      pthread_mutex_unlock( &ncurses_mutex);
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
  static unsigned char *receiveBuffer = NULL;	// the buffer inwhich to stick our incomming characters
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
      if( cmd != NULL) {
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
      }
      if( cmd->pcmd.Request == VR_PMAC_SENDCTRLCHAR)
	ls_pmac_state = LS_PMAC_STATE_WACK_CC;
      else if( cmd->pcmd.Request == VR_PMAC_GETMEM)
	ls_pmac_state = LS_PMAC_STATE_GMR;
      else if( cmd->no_reply == 0)
	ls_pmac_state = LS_PMAC_STATE_WACK;
      else
	ls_pmac_state = LS_PMAC_STATE_WACK_NFR;
      break;

    case LS_PMAC_STATE_CR:
      nsent = send( evt->fd, &cr_cmd, pmac_cmd_size, 0);
      gettimeofday( &pmac_time_sent, NULL);
      ls_pmac_state = LS_PMAC_STATE_WCR;
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
      unsigned char *newbuff;

      receiveBufferSize += 1400;
      newbuff = calloc( receiveBufferSize, sizeof( unsigned char));
      if( newbuff == NULL) {
	lslogging_log_message( "Out of memory");
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
			    unsigned char *buff		/**< [in] The buffer of bytes			*/
			    ) {

  char *sp;	// pointer to the command this is a reply to

  if( nreceived < 1400)
    buff[nreceived]=0;

  sp = (char *)(cmd->pcmd.bData);

  if( *buff == 0) {
    pthread_mutex_lock( &ncurses_mutex);
    wprintw( term_output, "%s\n", sp);
    pthread_mutex_unlock( &ncurses_mutex);
  } else {
    pthread_mutex_lock( &ncurses_mutex);
    wprintw( term_output, "%s: ", sp);
    pthread_mutex_unlock( &ncurses_mutex);
    cleanstr( buff);
  }
  wnoutrefresh( term_output);
  wnoutrefresh( term_input);
  doupdate();

  memset( cmd->pcmd.bData, 0, sizeof( cmd->pcmd.bData));
}

/** Receive a reply to a control character
 *  Print a "printable" version of the character to the terminal
 *  Followed by a hex dump of the response.
 */
void lspmac_SendControlReplyPrintCB(
				    pmac_cmd_queue_t *cmd,	/**< [in] Queue item this is a reply to		*/
				    int nreceived,		/**< [in] Number of bytes received		*/
				    unsigned char *buff		/**< [in] Buffer of bytes received		*/
				    ) {
  pthread_mutex_lock( &ncurses_mutex);
  wprintw( term_output, "control-%c: ", '@'+ ntohs(cmd->pcmd.wValue));
  pthread_mutex_unlock( &ncurses_mutex);
  hex_dump( nreceived, buff);
  pthread_mutex_lock( &ncurses_mutex);
  wnoutrefresh( term_output);
  wnoutrefresh( term_input);
  doupdate();
  pthread_mutex_unlock( &ncurses_mutex);
}


/** Service a reply to the getmem command.
 *  Not currently used.
 * \param cmd Queue item this is a reply to
 * \param nreceived Number of bytes received
 * \param buff Buffer of bytes recieved
 *  
 */
void lspmac_GetmemReplyCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {

  memcpy( &(dbmem[ntohs(cmd->pcmd.wValue)]), buff, nreceived);

  dbmemIn += nreceived;
  if( dbmemIn >= sizeof( dbmem)) {
    dbmemIn = 0;
  }
}

/** Request a chunk of memory to be returned.
 *  Not currently used
 */
pmac_cmd_queue_t *lspmac_SockGetmem(
				    int offset,			/**< [in] Offset in PMAC Double Buffer		*/
				    int nbytes			/**< [in] Number of bytes to request		*/
				    )  {
  return lspmac_send_command( VR_UPLOAD,   VR_PMAC_GETMEM, offset, 0, nbytes, NULL, lspmac_GetmemReplyCB, 0);
}

/** Send a one line command.
 *  Uses printf style arguments.
 */
pmac_cmd_queue_t *lspmac_SockSendline( 
				      char *fmt,		/**< [in] Printf style format string			*/
				      ...			/*        other arguments required by format string	*/
				       ) {
  va_list arg_ptr;
  char payload[1400];

  va_start( arg_ptr, fmt);
  vsnprintf( payload, sizeof(payload)-1, fmt, arg_ptr);
  payload[ sizeof(payload)-1] = 0;
  va_end( arg_ptr);

  lslogging_log_message( payload);

  return lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( payload), payload, lspmac_GetShortReplyCB, 0);
}

/** Send a command and ignore the response
 */
pmac_cmd_queue_t *lspmac_SockSendline_nr(
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

  return lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( s), s, NULL, 1);
}

/** Send a control character
 */
pmac_cmd_queue_t *lspmac_SockSendControlCharPrint(
						  char c	/**< The control character to send			*/
						  ) {
  return lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDCTRLCHAR, c, 0, 0, NULL, lspmac_SendControlReplyPrintCB, 0);
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
  char s[512];
  int pos, changed;

  pthread_mutex_lock( &(mp->mutex));

  pos = (*(mp->read_ptr) & mp->read_mask) == 0 ? 0 : 1;

  changed = pos != mp->position;
  mp->position = pos;

  // Not sure what kind of status makes sense to report
  mp->statuss[0] = 0;
  pthread_mutex_unlock( &(mp->mutex));

  if( changed)
    lsevents_send_event( "%s %d", mp->name, pos);
}

/** Read a DAC motor position
 */
void lspmac_dac_read(
		     lspmac_motor_t *mp		/**< [in] The motor			*/
		     ) {
  int pos;
  double u2c;

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

  // Not sure what kind of status makes sense to report
  mp->statuss[0] = 0;

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

  if( md2_status.fs_is_open) {
    mvwprintw( term_status2, 1, 1, "Shutter Open  ");
    mp->position = 1;
  } else {
    mvwprintw( term_status2, 1, 1, "Shutter Closed");
    mp->position = 0;
  }

  // Not sure what kind of status makes sense to report
  mp->statuss[0] = 0;

  pthread_mutex_unlock( &lspmac_shutter_mutex);
}

/** Home the motor.
 */
void lspmac_home1_queue(
			lspmac_motor_t *mp			/**< [in] motor we are concerned about		*/
			) {
  char openloops[32];
  char *sp;
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
  // system.
  //
  if( coord_num > 0) {
    for( i=0; i<lspmac_nmotors; i++) {
      if( &(lspmac_motors[i]) == mp)
	continue;
      if( lsredis_getl(lspmac_motors[i].coord_num) == coord_num) {
	if( lspmac_motors[i].homing) {
	  pthread_mutex_unlock( &(mp->mutex));
	  return;
	}
      }
    }
  }
  mp->homing = 1;
       
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
    snprintf( openloops, sizeof(openloops)-1, "#%d$*", motor_num);
    openloops[sizeof(openloops)-1] = 0;
    lspmac_SockSendline( openloops);
  }

  pthread_mutex_unlock( &(mp->mutex));
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

    pthread_mutex_lock( &ncurses_mutex);
    wprintw( term_output, "home2 is queuing '%s'\n", *spp);
    wnoutrefresh( term_output);
    doupdate();
    pthread_mutex_unlock( &ncurses_mutex);

    lspmac_SockSendline( *spp);
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
  int motor_num;
  char *fmt;
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
  u2c       = lsredis_getd( mp->u2c);
  motor_num = lsredis_getl( mp->motor_num);

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
      lslogging_log_message("lspmac_motor_read: omega zero secs %d  nsecs %d ozt.tv_sec %ld  ozt.tv_nsec  %ld, motor cnts %d",
			    secs, nsecs, omega_zero_time.tv_sec, omega_zero_time.tv_nsec, *mp->actual_pos_cnts_p);
    }
    omega_zero_search = 0;
  }


  // Make local copies so we can inspect them in other threads
  // without having to grab the status mutex
  //
  mp->status1 = *mp->status1_p;
  mp->status2 = *mp->status2_p;
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

  mvwprintw( mp->win, 2, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, " ");
  mvwprintw( mp->win, 2, 1, "%*d cts", LS_DISPLAY_WINDOW_WIDTH-6, mp->actual_pos_cnts);
  mvwprintw( mp->win, 3, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, " ");

  if( mp->nlut >0 && mp->lut != NULL) {
    mp->position = lspmac_rlut( mp->nlut, mp->lut, mp->actual_pos_cnts);
  } else {
    if( u2c != 0.0) {
      mp->position = mp->actual_pos_cnts / u2c;
    } else {
      mp->position = mp->actual_pos_cnts;
    }
  }

  fmt = lsredis_getstr( mp->printf_fmt);
  snprintf( s, sizeof(s)-1, fmt, 8, mp->position);
  free( fmt);

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
  if( mp->homing == 2 && (mp->status2 & 0x000400 != 0) && (mp->status2 & 0x000001 != 0))
    mp->homing = 0;


  s[sizeof(s)-1] = 0;
  mvwprintw( mp->win, 3, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-6, s);

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
  else if( mp->status2 & 0x000001)
    sp = "In Position";

  mvwprintw( mp->win, 6, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, sp);
  wnoutrefresh( mp->win);
  
  strncpy( mp->statuss, sp, sizeof( mp->statuss)-1);
  mp->statuss[sizeof(mp->statuss)-1] = 0;

  pthread_mutex_unlock( &(mp->mutex));

  if( homing1)
    lspmac_home1_queue( mp);

  if( homing2)
    lspmac_home2_queue( mp);

  lspmac_status_last_time.tv_sec  = lspmac_status_time.tv_sec;
  lspmac_status_last_time.tv_nsec = lspmac_status_time.tv_nsec;
}

/** Service routing for status upate
 *  This updates positions and status information.
 */
void lspmac_get_status_cb(
			  pmac_cmd_queue_t *cmd,		/**< [in] The command that generated this reply	*/
			  int nreceived,			/**< [in] Number of bytes received		*/
			  unsigned char *buff			/**< [in] The Big Byte Buffer			*/
			  ) {
  static int cnt = 0;
  static char s[256];
  static struct timeval ts1, ts2;

  char *sp;
  int i, pos;
  lspmac_motor_t *mp;
  lspmac_bi_t    *bp;

  clock_gettime( CLOCK_REALTIME, &lspmac_status_time);

  if( cnt == 0) {
    gettimeofday( &ts1, NULL);
  }

  pthread_mutex_lock( &md2_status_mutex);
  memcpy( &md2_status, buff, sizeof(md2_status));
  pthread_mutex_unlock( &md2_status_mutex);


  //
  // track the coordinate system moving flags
  //
  pthread_mutex_lock( &lspmac_moving_mutex);
  if( md2_status.moving_flags != lspmac_moving_flags) {
    lslogging_log_message( "lspmac_get_status_cb: new moving flag: %0x", md2_status.moving_flags);
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

    pos = (*(bp->ptr) & bp->mask) == 0 ? 0 : 1;

    if( bp->first_time) {
      bp->first_time = 0;
      if( pos==1 && bp->changeEventOn != NULL && bp->changeEventOn[0] != 0)
	lsevents_send_event( lspmac_bis[i].changeEventOn);
      if( pos==0 && bp->changeEventOff != NULL && bp->changeEventOff[0] != 0)
	lsevents_send_event( lspmac_bis[i].changeEventOff);
    } else {
      if( pos != bp->previous) {
	if( pos==1 && bp->changeEventOn != NULL && bp->changeEventOn[0] != 0)
	  lsevents_send_event( lspmac_bis[i].changeEventOn);
	if( pos==0 && bp->changeEventOff != NULL && bp->changeEventOff[0] != 0)
	  lsevents_send_event( lspmac_bis[i].changeEventOff);
      }
    }
    bp->previous = pos;
    pthread_mutex_unlock( &(bp->mutex));
  }

  pthread_mutex_lock( &ncurses_mutex);

  // acc11c_1
  // mask  bit
  // 0x01  0	Air pressure OK
  // 0x02  1	Air bearing OK
  // 0x04  2	Cryo switch
  // 0x08  3
  // 0x10  4
  // 0x20  5
  // 0x40  6	Cryo is back

  //
  // acc11c_2
  // mask  bit
  // 0x01  0	Fluor Dector back
  // 0x02  1	Sample Detected
  // 0x04  2
  // 0x08  3
  // 0x10  4
  // 0x20  5	Etel Ready
  // 0x40  6	Etel On
  // 0x80  7	Etel Init OK

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


  // acc11c_3
  // mask  bit
  // 0x01  0	Minikappa OK
  // 0x02  1
  // 0x04  2
  // 0x08  3	Arm Parked

  // acc11c_5
  // mask  bit
  // 0x01  0    Mag Off
  // 0x02  1    Condenser Out
  // 0x04  2    Cryo Back
  // 0x08  3    Dryer On
  // 0x10  4    FluoDet Out
  // 0x20  5
  // 0x40  6    1=SmartMag, 0=Permanent Mag
  //

  if( md2_status.acc11c_5 & 0x04)
    mvwprintw( term_status2, 3, 1, "%*s", -8, "Cryo Out");
  else
    mvwprintw( term_status2, 3, 1, "%*s", -8, "Cryo In ");

  // acc11c_6
  // mask   bit
  // 0x0080   7   Etel Enable
  // 0x0100   8   Fast Shutter Enable
  // 0x0200   9   Fast Shutter Manual Enable
  // 0x0400  10   Fast Shutter On



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

  /*
  if( ++cnt % 1000 == 0) {
    gettimeofday( &ts2, NULL);

    lslogging_log_message( "Refresh Rate: %0.1f Hz", 1000000.*(cnt)/(ts2.tv_sec*1000000 + ts2.tv_usec - ts1.tv_sec*1000000 - ts1.tv_usec));

    cnt = 0;
  }
  */
}

/** Request a status update from the PMAC
 */
void lspmac_get_status() {
  lspmac_send_command( VR_UPLOAD, VR_PMAC_GETMEM, 0x400, 0, sizeof(md2_status_t), NULL, lspmac_get_status_cb, 0);
}


/** Receive the values of all the I variables
 *  Update our Postgresql database with the results
 */
void lspmac_GetAllIVarsCB(
			  pmac_cmd_queue_t *cmd,	/**< [in] The command that gave this response	*/
			  int nreceived,		/**< [in] Number of bytes received		*/
			  unsigned char *buff		/**< [in] The byte buffer			*/
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
  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( cmds), cmds, lspmac_GetAllIVarsCB, 0);
}

/** Receive the values of all the M variables
 *  Update our database with the results
 */
void lspmac_GetAllMVarsCB(
			  pmac_cmd_queue_t *cmd,	/**< [in] The command that started this		*/
			  int nreceived,		/**< [in] Number of bytes received		*/
			  unsigned char *buff		/**< [in] Our byte buffer			*/
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
  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( cmds), cmds, lspmac_GetAllMVarsCB, 0);
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

  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen(tmps), tmps, NULL, 0);
}



/** PMAC command with call back
 */
void lspmac_sendcmd(
			void (*responseCB)(pmac_cmd_queue_t *, int, unsigned char *),		/**< [in] our callback routine                 */
			char *fmt,								/**< [in] printf style format string           */
			...									/*        Arguments specified by format string */
			) {
  static char tmps[1024];
  va_list arg_ptr;

  va_start( arg_ptr, fmt);
  vsnprintf( tmps, sizeof(tmps)-1, fmt, arg_ptr);
  tmps[sizeof(tmps)-1]=0;
  va_end( arg_ptr);

  lspmac_send_command( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen(tmps), tmps, responseCB, 0);
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
  // These state require that we listen for packets
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
  // These state require that we send packets out.
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

  while( 1) {
    int pollrtn;

    lspmac_next_state();

    if( pmacfd.fd == -1) {
      sleep( 10);	// The pmac is not connected.  Should we warn someone?
      //
      // This just puts us into a holding pattern until the pmac becomes connected again
      //
      // TODO:
      // Check PMAC initialization logic and our queues to ensure that it is sane to
      // re-initialize things.  Probably bad things will happen.
      //
      continue;
    }

    pollrtn = poll( &pmacfd, 1, 10);
    if( pollrtn) {
      lspmac_Service( &pmacfd);
    }
  }
}





/** Move method for dac motor objects (ie, lights)
 */
void lspmac_movedac_queue(
			  lspmac_motor_t *mp,		/**< [in] Our motor						*/
			  double requested_position	/**< [in] Desired x postion (look up and send y position)	*/
			  ) {
  char s[512];
  double y;
  double u2c;

  pthread_mutex_lock( &(mp->mutex));

  u2c = lsredis_getd( mp->u2c);
  mp->requested_position = requested_position;

  if( mp->nlut > 0 && mp->lut != NULL) {
    mp->requested_pos_cnts = u2c * lspmac_lut( mp->nlut, mp->lut, requested_position);
    mp->not_done    = 1;
    mp->motion_seen = 0;


    //
    //  By convention requested_pos_cnts scales from 0 to 100
    //  for the lights u2c converts this to 0 to 16,000
    //  for the scintilator focus this is   0 to 32,000
    //
    snprintf( s, sizeof(s)-1, "%s=%d", mp->dac_mvar, mp->requested_pos_cnts);
    mp->pq = lspmac_SockSendline_nr( s);

  }

  pthread_mutex_unlock( &(mp->mutex));
}


/** Move method for the zoom motor
 */
void lspmac_movezoom_queue(
			   lspmac_motor_t *mp,			/**< [in] the zoom motor		*/
			   double requested_position		/**< [in] our desired zoom		*/
			   ) {
  char s[512];
  double y;
  int motor_num;

  pthread_mutex_lock( &(mp->mutex));

  motor_num = lsredis_getl( mp->motor_num);

  mp->requested_position = requested_position;

  if( mp->nlut > 0 && mp->lut != NULL) {
    y = lspmac_lut( mp->nlut, mp->lut, requested_position);

    mp->requested_pos_cnts = (int)y;
    mp->not_done    = 1;
    mp->motion_seen = 0;


    snprintf( s, sizeof(s)-1, "#%d j=%d", motor_num, mp->requested_pos_cnts);
    mp->pq = lspmac_SockSendline_nr( s);

  }
  pthread_mutex_unlock( &(mp->mutex));

}

/** Move a given motor to one of its preset positions.
 *  No movement if the preset is not found.
 *  \param mp lspmac motor pointer
 *  \param name Name of the preset to use
 */
void lspmac_move_preset_queue( lspmac_motor_t *mp, char *preset_name) {
  double pos;
  int err;

  lslogging_log_message( "lspmac_move_preset_queue: Called with motor %s and preset named '%s'", mp->name, preset_name);

  err = lsredis_find_preset( mp->name, preset_name, &pos);
  if( err == 0)
    return;

  mp->moveAbs( mp, pos);
  lslogging_log_message( "lspmac_move_preset_queue: moving %s to preset '%s' (%f)", mp->name, preset_name, pos);
}



/** Move method for the fast shutter
 *
 *  Slightly more complicated than a binary io as some flags need
 *  to be set up.
 */
void lspmac_moveabs_fshut_queue(
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
    mp->pq = lspmac_SockSendline_nr( "M1124=0 M1125=1 M1126=1");
  } else {
    //
    // ManualOn=0, ManualEnable=0, ScanEnable=1
    //
    mp->pq = lspmac_SockSendline_nr( "M1126=0 M1125=0 M1124=1");
  }

  pthread_mutex_unlock( &(mp->mutex));
}

/** Move method for binary i/o motor objects
 */
void lspmac_moveabs_bo_queue(
			      lspmac_motor_t *mp,		/**< [in] A binary i/o motor object		*/
			      double requested_position		/**< [in] a 1 or a 0 request to move		*/
			      ) {


  pthread_mutex_lock( &(mp->mutex));
  mp->requested_position = requested_position == 0.0 ? 0.0 : 1.0;
  mp->requested_pos_cnts = requested_position == 0.0 ? 0 : 1;

  mp->not_done    = 1;
  mp->motion_seen = 0;
  mp->pq = lspmac_SockSendline_nr( mp->write_fmt, mp->requested_pos_cnts);


  pthread_mutex_unlock( &(mp->mutex));

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
  char s[512];	 // PMAC command string buffer
  double u2c;
  double max_accel;

  pthread_mutex_lock( &(mp->mutex));

  u2c       = lsredis_getd( mp->u2c);
  max_accel = lsredis_getd( mp->max_accel);
  coord_num = lsredis_getl( mp->coord_num);

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
  mp->requested_pos_cnts = u2c * mp->requested_position;
  q10 = mp->requested_pos_cnts;
  q11 = u2c * delta;
  q12 = 1000 * time;
  q13 = q11 / q12 / max_accel;
  q100 = 1 << (coord_num - 1);
  pthread_mutex_unlock( &(mp->mutex));

  snprintf( s, sizeof(s)-1, "&%d Q10=%d Q11=%d Q12=%d Q13=%d Q100=%d B240R", coord_num, q10, q11, q12, q13, q100);
  pthread_mutex_lock( &(mp->mutex));
  mp->pq = lspmac_SockSendline_nr( s);
  pthread_mutex_unlock( &(mp->mutex));
}

/** "move" frontlight on/off
 */
void lspmac_moveabs_frontlight_oo_queue( lspmac_motor_t *mp, double pos) {
  pthread_mutex_lock( &(mp->mutex));
  *mp->actual_pos_cnts_p = pos;
  mp->position =           pos;
  pthread_mutex_unlock( &(mp->mutex));
  if( pos == 0.0) {
    flight->moveAbs( flight, 0.0);
  } else {
    flight->moveAbs( flight, lspmac_getPosition( zoom));
  }
}

void lspmac_moveabs_flight_factor_queue( lspmac_motor_t *mp, double pos) {
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
  }
}

void lspmac_moveabs_blight_factor_queue( lspmac_motor_t *mp, double pos) {
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
}


/** Special motion program to collect centering video
 */
void lspmac_video_rotate( double secs) {
  double q10;		// starting position (counts)
  double q11;		// delta counts
  double q12;		// milliseconds to run over delta
  
  double u2c;

  if( secs <= 0.0)
    return;

  omega_zero_search = 1;

  pthread_mutex_lock( &(omega->mutex));
  u2c = lsredis_getd( omega->u2c);

  q10 = 0;
  q11 = 360.0 * u2c;
  q12 = 1000 * secs;
  

  omega_zero_velocity = 360.0 * u2c / secs;	// counts/second to back calculate zero crossing time

  omega->pq = lspmac_SockSendline_nr( "&1 Q10=%.1f Q11=%.1f Q12=%.1f Q13=(I117) Q14=(I116) B240R", q10, q11, q12);
  pthread_mutex_unlock( &(omega->mutex));
}


/** Move method for normal stepper and servo motor objects
 */
void lspmac_move_or_jog_abs_queue(
			  lspmac_motor_t *mp,			/**< [in] The motor to move			*/
			  double requested_position,		/**< [in] Where to move it			*/
			  int use_jog				/**< [in] 1 to force jog, 0 for motion prog	*/
			  ) {
  char s[512];			//!< buffer to send to pmac
  int q100;			//!< coordinate system bit
  int requested_pos_cnts;	//!< the requested position in units of "counts"
  int coord_num, motor_num;	//!< motor and coordinate system;
  char *axis;			//!< our axis
  double u2c;

  pthread_mutex_lock( &(mp->mutex));

  u2c       = lsredis_getd(   mp->u2c);
  motor_num = lsredis_getl(   mp->motor_num);
  coord_num = lsredis_getl(   mp->coord_num);
  axis      = lsredis_getstr( mp->axis);

  if( u2c == 0.0) {
    //
    // Shouldn't try moving a motor that has no units defined
    //
    pthread_mutex_unlock( &(mp->mutex));
    return;
  }
  mp->requested_position = requested_position;
  mp->not_done    = 1;
  mp->motion_seen = 0;
  mp->requested_pos_cnts = u2c * requested_position;  
  requested_pos_cnts = mp->requested_pos_cnts;

  if( use_jog || axis == NULL || *axis == 0) {
    use_jog = 1;
  } else {
    use_jog = 0;
    q100 = 1 << (coord_num -1);
  }

  pthread_mutex_unlock( &(mp->mutex));

  if( use_jog) {
    snprintf( s, sizeof(s)-1, "#%d j=%d", motor_num, requested_pos_cnts);
  } else {

    //
    // Make sure the coordinate system is not moving something, wait if it is
    // TODO: put in a timeout so we have a way out if something goes wrong
    // TODO: are we sure this thread is not the one moving it?
    //
    pthread_mutex_lock( &lspmac_moving_mutex);
    lslogging_log_message( "lspmac_moveabs_queue: waiting for previous moves to end.  lspmac_moving_flags = %0x", lspmac_moving_flags);
    while( (lspmac_moving_flags & q100) != 0)
      pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);
    pthread_mutex_unlock( &lspmac_moving_mutex);
    lslogging_log_message( "lspmac_moveabs_queue: Done.  lspmac_moving_flags = %0x", lspmac_moving_flags);

    //
    // Set the "we are moving this coordinate system" flag
    //
    lspmac_SockSendline( "M5075=(M5075 | %d)", q100);
    
    switch( *axis) {
    case 'A':
      snprintf( s, sizeof(s)-1, "&%d Q16=%d Q100=%d B146R", coord_num, requested_pos_cnts, q100);
      break;

    case 'B':
      snprintf( s, sizeof(s)-1, "&%d Q17=%d Q100=%d B147R", coord_num, requested_pos_cnts, q100);
      break;

    case 'C':
      snprintf( s, sizeof(s)-1, "&%d Q18=%d Q100=%d B148R", coord_num, requested_pos_cnts, q100);
      break;
    case 'X':
      snprintf( s, sizeof(s)-1, "&%d Q10=%d Q100=%d B140R", coord_num, requested_pos_cnts, q100);
      break;

    case 'Y':
      snprintf( s, sizeof(s)-1, "&%d Q11=%d Q100=%d B141R", coord_num, requested_pos_cnts, q100);
      break;

    case 'Z':
      snprintf( s, sizeof(s)-1, "&%d Q12=%d Q100=%d B142R", coord_num, requested_pos_cnts, q100);
      break;

    case 'U':
      snprintf( s, sizeof(s)-1, "&%d Q13=%d Q100=%d B143R", coord_num, requested_pos_cnts, q100);
      break;

    case 'V':
      snprintf( s, sizeof(s)-1, "&%d Q14=%d Q100=%d B144R", coord_num, requested_pos_cnts, q100);
      break;

    case 'W':
      snprintf( s, sizeof(s)-1, "&%d Q15=%d Q100=%d B145R", coord_num, requested_pos_cnts, q100);
      break;
    }

    //
    // Make sure the flag has been seen
    //
    pthread_mutex_lock( &lspmac_moving_mutex);
    lslogging_log_message( "lspmac_moveabs_queue: waiting for moving flag to propagate.  lspmac_moving_flags = %0x", lspmac_moving_flags);
    while( (lspmac_moving_flags & q100) == 0)
      pthread_cond_wait( &lspmac_moving_cond, &lspmac_moving_mutex);
    pthread_mutex_unlock( &lspmac_moving_mutex);
    lslogging_log_message( "lspmac_moveabs_queue: Done.  lspmac_moving_flags = %0x", lspmac_moving_flags);
  }
  pthread_mutex_lock( &(mp->mutex));
  mp->pq = lspmac_SockSendline_nr( s);
  pthread_mutex_unlock( &(mp->mutex));

  free( axis);
}

/** move using a preset value
 */
void lspmac_move_or_jog_preset_queue(
				     lspmac_motor_t *mp,	/**< [in] Our motor				*/
				     char *preset,		/**< [in] the name of the preset		*/
				     int use_jog		/**< [in[ 1 to force jog, 0 to try motion prog	*/
				     ) {
  double pos;
  int err;

  if( preset == NULL || *preset == 0)
    return;

  err = lsredis_find_preset( mp->name, preset, &pos);

  if( err != 0)
    lspmac_move_or_jog_abs_queue( mp, pos, use_jog);
}


/** Use coordinate system motion program, if available, to move motor to requested position
 */
void lspmac_moveabs_queue(
			  lspmac_motor_t *mp,			/**< [in] The motor to move			*/
			  double requested_position		/**< [in] Where to move it			*/
			  ) {
  
  lspmac_move_or_jog_abs_queue( mp, requested_position, 0);
}

/** Use jog to move motor to requested position
 */
void lspmac_jogabs_queue(
			  lspmac_motor_t *mp,			/**< [in] The motor to move			*/
			  double requested_position		/**< [in] Where to move it			*/
			  ) {
  
  lspmac_move_or_jog_abs_queue( mp, requested_position, 1);
}


/** Wait for motor to finish moving.
 *  Assume motion already queued, now just wait
 */
void lspmac_moveabs_wait(
			 lspmac_motor_t *mp		/**< [in] The motor object to wait for		*/
			 ) {
  struct timespec wt;
  int return_code;
  pmac_cmd_queue_t *pq;

  //
  // Copy the queue item for the most recent move request
  //
  pthread_mutex_lock( &(mp->mutex));
  pq = mp->pq;
  pthread_mutex_unlock( &(mp->mutex));

  pthread_mutex_lock( &pmac_queue_mutex);
  //
  // wait for the command to be sent
  //
  while( pq->time_sent.tv_sec==0)
    pthread_cond_wait( &pmac_queue_cond, &pmac_queue_mutex);

  //
  // set the timeout to be long enough after we sent the motion request to ensure that
  // we will have read back the motor moving status but not so long that the timeout causes
  // problems;
  //
  wt.tv_sec  = pq->time_sent.tv_sec;
  wt.tv_nsec = pq->time_sent.tv_nsec + 500000000;

  pthread_mutex_unlock( &pmac_queue_mutex);

  if( wt.tv_nsec >= 1000000000) {
    wt.tv_nsec -= 1000000000;
    wt.tv_sec += 1;
  }

  //
  // wait for the motion to have started
  // This will time out if the motion ends before we can read the status back
  // hence the added complication of time stamp of the sent packet.
  //

  return_code=0;

  pthread_mutex_lock( &(mp->mutex));
  while( mp->motion_seen == 0 && return_code == 0)
    return_code = pthread_cond_timedwait( &(mp->cond), &(mp->mutex), &wt);

  if( return_code == 0) {
    //
    // wait for the motion that we know has started to finish
    //
    while( mp->not_done)
      pthread_cond_wait( &(mp->cond), &(mp->mutex));
  }

  //
  // if return code was not 0 then we know we shouldn't wait for not_done flag.
  // In this case the motion ended before we read the status registers
  //
  pthread_mutex_unlock( &(mp->mutex));

}

/** Helper funciton for the init calls
 */
void _lspmac_motor_init( lspmac_motor_t *d, char *name) {
  lspmac_nmotors++;

  pthread_mutex_init( &(d->mutex), NULL);
  pthread_cond_init(  &(d->cond), NULL);

  d->name                = strdup(name);
  d->u2c                 = lsredis_get_obj( "%s.u2c",               d->name);
  d->printf_fmt		 = lsredis_get_obj( "%s.printf",            d->name);
  d->redis_fmt		 = lsredis_get_obj( "%s.format",            d->name);
  d->unit		 = lsredis_get_obj( "%s.unit",              d->name);
  d->max_speed		 = lsredis_get_obj( "%s.max_speed",         d->name);
  d->max_accel		 = lsredis_get_obj( "%s.max_accel",         d->name);
  d->motor_num		 = lsredis_get_obj( "%s.motor_num",         d->name);
  d->coord_num		 = lsredis_get_obj( "%s.coord_num",         d->name);
  d->axis		 = lsredis_get_obj( "%s.axis",	            d->name);
  d->home		 = lsredis_get_obj( "%s.home",	            d->name);
  d->active		 = lsredis_get_obj( "%s.active",	    d->name);
  d->active_init	 = lsredis_get_obj( "%s.active_init",	    d->name);
  d->inactive_init	 = lsredis_get_obj( "%s.inactive_init",	    d->name);

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
				  void (*moveAbs)(lspmac_motor_t *,double)	/**< [in] Method to use to move this motor		*/
				  ) {

  _lspmac_motor_init( d, name);

  d->moveAbs             = moveAbs;
  d->read                = lspmac_pmacmotor_read;
  d->actual_pos_cnts_p   = posp;
  d->status1_p           = stat1p;
  d->status2_p           = stat2p;
  d->win = newwin( LS_DISPLAY_WINDOW_HEIGHT, LS_DISPLAY_WINDOW_WIDTH, wy*LS_DISPLAY_WINDOW_HEIGHT, wx*LS_DISPLAY_WINDOW_WIDTH);
  box( d->win, 0, 0);
  mvwprintw( d->win, 1, 1, "%s", wtitle);
  wnoutrefresh( d->win);

  return d;
}

/** Initalize the fast shutter motor
 */
lspmac_motor_t *lspmac_fshut_init(
				  lspmac_motor_t *d		/**< [in] Our uninitialized motor object	*/
				  ) {

  _lspmac_motor_init( d, "fastShutter");

  d->moveAbs           = lspmac_moveabs_fshut_queue;
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
  d->read              = lspmac_bo_read;
  d->write_fmt         = strdup( write_fmt);
  d->read_ptr	       = read_ptr;
  d->read_mask         = read_mask;

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
				void (*moveAbs)(lspmac_motor_t *,double)	/**< [in] Method to use to move this motor		*/
				) {

  _lspmac_motor_init( d, name);
  d->moveAbs           = moveAbs;
  d->read              = lspmac_dac_read;
  d->actual_pos_cnts_p = posp;
  d->dac_mvar          = strdup(mvar);

  return d;
}

/** Dummy routine to read a soft motor
 */
void lspmac_soft_motor_read( lspmac_motor_t *p) {

}


lspmac_motor_t *lspmac_soft_motor_init( lspmac_motor_t *d, char *name, void (*moveAbs)(lspmac_motor_t *, double)) {

  _lspmac_motor_init( d, name);

  d->moveAbs      = moveAbs;
  d->read         = lspmac_soft_motor_read;
  d->actual_pos_cnts_p = calloc( sizeof(int), 1);
  *d->actual_pos_cnts_p = 0;
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
}



/** Initialize this module
 */
void lspmac_init(
		 int ivarsflag,		/**< [in]  Set global flag to harvest i variables			*/
		 int mvarsflag		/**< [in]  Set global flag to harvest m variables			*/
		 ) {
  md2_status_t *p;

  // Set our global harvest flags
  getivars = ivarsflag;
  getmvars = mvarsflag;

  // All important status mutex
  pthread_mutex_init( &md2_status_mutex, NULL);

  //
  // Get the MD2 initialization strings
  //
  lspmac_md2_init = lsredis_get_obj( "md2_pmac.init");

  //
  // Initialize the motor objects
  //

  p = &md2_status;

  omega  = lspmac_motor_init( &(lspmac_motors[ 0]), 0, 0, &p->omega_act_pos,     &p->omega_status_1,     &p->omega_status_2,     "Omega   #1 &1 A", "omega",       lspmac_moveabs_queue);
  alignx = lspmac_motor_init( &(lspmac_motors[ 1]), 0, 1, &p->alignx_act_pos,    &p->alignx_status_1,    &p->alignx_status_2,    "Align X #2 &3 X", "align.x",     lspmac_moveabs_queue);
  aligny = lspmac_motor_init( &(lspmac_motors[ 2]), 0, 2, &p->aligny_act_pos,    &p->aligny_status_1,    &p->aligny_status_2,    "Align Y #3 &3 Y", "align.y",     lspmac_moveabs_queue);
  alignz = lspmac_motor_init( &(lspmac_motors[ 3]), 0, 3, &p->alignz_act_pos,    &p->alignz_status_1,    &p->alignz_status_2,    "Align Z #4 &3 Z", "align.z",     lspmac_moveabs_queue);
  anal   = lspmac_motor_init( &(lspmac_motors[ 4]), 0, 4, &p->analyzer_act_pos,  &p->analyzer_status_1,  &p->analyzer_status_2,  "Anal    #5",      "lightPolar",  lspmac_moveabs_queue);
  zoom   = lspmac_motor_init( &(lspmac_motors[ 5]), 1, 0, &p->zoom_act_pos,      &p->zoom_status_1,      &p->zoom_status_2,      "Zoom    #6 &4 Z", "cam.zoom",    lspmac_movezoom_queue);
  apery  = lspmac_motor_init( &(lspmac_motors[ 6]), 1, 1, &p->aperturey_act_pos, &p->aperturey_status_1, &p->aperturey_status_2, "Aper Y  #7 &5 Y", "appy",        lspmac_moveabs_queue);
  aperz  = lspmac_motor_init( &(lspmac_motors[ 7]), 1, 2, &p->aperturez_act_pos, &p->aperturez_status_1, &p->aperturez_status_2, "Aper Z  #8 &5 Z", "appz",        lspmac_moveabs_queue);
  capy   = lspmac_motor_init( &(lspmac_motors[ 8]), 1, 3, &p->capy_act_pos,      &p->capy_status_1,      &p->capy_status_2,      "Cap Y   #9 &5 U", "capy",        lspmac_moveabs_queue);
  capz   = lspmac_motor_init( &(lspmac_motors[ 9]), 1, 4, &p->capz_act_pos,      &p->capz_status_1,      &p->capz_status_2,      "Cap Z  #10 &5 V", "capz",        lspmac_moveabs_queue);
  scint  = lspmac_motor_init( &(lspmac_motors[10]), 2, 0, &p->scint_act_pos,     &p->scint_status_1,     &p->scint_status_2,     "Scin Z #11 &5 W", "scint",       lspmac_moveabs_queue);
  cenx   = lspmac_motor_init( &(lspmac_motors[11]), 2, 1, &p->centerx_act_pos,   &p->centerx_status_1,   &p->centerx_status_2,   "Cen X  #17 &2 X", "centering.x", lspmac_moveabs_queue);
  ceny   = lspmac_motor_init( &(lspmac_motors[12]), 2, 2, &p->centery_act_pos,   &p->centery_status_1,   &p->centery_status_2,   "Cen Y  #18 &2 Y", "centering.y", lspmac_moveabs_queue);
  kappa  = lspmac_motor_init( &(lspmac_motors[13]), 2, 3, &p->kappa_act_pos,     &p->kappa_status_1,     &p->kappa_status_2,     "Kappa  #19 &7 X", "kappa",       lspmac_moveabs_queue);
  phi    = lspmac_motor_init( &(lspmac_motors[14]), 2, 4, &p->phi_act_pos,       &p->phi_status_1,       &p->phi_status_2,       "Phi    #20 &7 Y", "phi",         lspmac_moveabs_queue);

  fshut  = lspmac_fshut_init( &(lspmac_motors[15]));
  flight = lspmac_dac_init( &(lspmac_motors[16]), &p->front_dac,   "M1200", "frontLight.intensity", lspmac_movedac_queue);
  blight = lspmac_dac_init( &(lspmac_motors[17]), &p->back_dac,    "M1201", "backLight.intensity",  lspmac_movedac_queue);
  fscint = lspmac_dac_init( &(lspmac_motors[18]), &p->scint_piezo, "M1203", "scint.focus",          lspmac_movedac_queue);

  blight_ud = lspmac_bo_init( &(lspmac_motors[19]), "backLight",  "M1101=%d", &(md2_status.acc11c_5), 0x02);
  cryo      = lspmac_bo_init( &(lspmac_motors[20]), "cryo",       "M1102=%d", &(md2_status.acc11c_5), 0x04);
  dryer     = lspmac_bo_init( &(lspmac_motors[21]), "dryer",      "M1103=%d", &(md2_status.acc11c_5), 0x08);
  fluo      = lspmac_bo_init( &(lspmac_motors[22]), "fluo",       "M1008=%d", &(md2_status.acc11c_2), 0x01);
  flight_oo = lspmac_soft_motor_init( &(lspmac_motors[23]), "frontLight",        lspmac_moveabs_frontlight_oo_queue);
  blight_f  = lspmac_soft_motor_init( &(lspmac_motors[24]), "backLight.factor",  lspmac_moveabs_blight_factor_queue);
  flight_f  = lspmac_soft_motor_init( &(lspmac_motors[25]), "frontLight.factor", lspmac_moveabs_flight_factor_queue);

  cryo_switch = lspmac_bi_init( &(lspmac_bis[0]), &(md2_status.acc11c_1), 0x04, "CryoSwitchChanged", "CryoSwitchChanged");

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

  pthread_mutex_init( &pmac_queue_mutex, NULL);
  pthread_cond_init(  &pmac_queue_cond, NULL);

  lspmac_shutter_state = 0;				// assume the shutter is now closed: not a big deal if we are wrong
  pthread_mutex_init( &lspmac_shutter_mutex, NULL);
  pthread_cond_init(  &lspmac_shutter_cond, NULL);
  pmacfd.fd = -1;

  pthread_mutex_init( &lspmac_moving_mutex, NULL);
  pthread_cond_init(  &lspmac_moving_cond, NULL);
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
void lspmac_scint_inPosition_cb( char *event) {
  double pos;
  double cover;
  int err;

  pthread_mutex_lock( &(scint->mutex));
  pos = scint->position;
  err = lsredis_find_preset( scint->name, "Cover", &pos);
  pthread_mutex_unlock( &(scint->mutex));

  lslogging_log_message( "lspmac_scint_inPosition_cb: pos %f, cover %f, diff %f, err %d", pos, cover, fabs( pos-cover), err);

  if( err == 0)
    return;

  if( fabs( pos - cover) <= 0.1) {
    dryer->moveAbs( dryer, 1.0);
    lslogging_log_message( "lspmac_scint_inPosition_cb: Starting dryer");
    lstimer_add_timer( "scintDried", 1, 120, 0);
  }
}

/** Turn on the backlight whenever it goes up
 *  \param event Name of the event that called us
 */
void lspmac_backLight_up_cb( char *event) {
  int z;

  blight->moveAbs( blight, lspmac_getPosition( zoom));
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
  double z;

  z = lspmac_getPosition( zoom);
  if( lspmac_getPosition( flight_oo) != 0.0) {
      flight->moveAbs( flight, z);
    } else {
      flight->moveAbs( flight, 0.0);
    }
  if( lspmac_getPosition( blight_ud) != 0.0) {
    blight->moveAbs( blight, z);
  } else {
    blight->moveAbs( blight, 0.0);
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
  double *dp;

  pthread_mutex_lock( &zoom->mutex);

  zoom->nlut = 10;
  zoom->lut = calloc( 2 * zoom->nlut, sizeof( double));
  if( zoom->lut == NULL) {
    lslogging_log_message( "lspmac_zoom_lut_setup: out of memory");
    exit( -1);
  }

  for( i=1, dp = zoom->lut; i<=zoom->nlut; i++, dp += 2) {
    p = lsredis_get_obj( "cam.zoom.%d.MotorPosition", i);
    if( p==NULL || strlen( lsredis_getstr(p)) == 0) {
      free( zoom->lut);
      zoom->nlut = 0;
      pthread_mutex_unlock( &zoom->mutex);
      lslogging_log_message( "lspmac_zoom_lut_setup: cannot find MotorPosition element for cam.zoom level %d", i);
      return;
    }
    *dp = i;
    *(dp+1) = lsredis_getd( p);
  }
  pthread_mutex_unlock( &zoom->mutex);
}

/** Set up lookup table for flight
 */
void lspmac_flight_lut_setup() {
  int i;
  lsredis_obj_t *p;
  double *dp;

  pthread_mutex_lock( &flight->mutex);

  flight->nlut = 11;
  flight->lut = calloc( 2 * flight->nlut, sizeof( double));
  if( flight->lut == NULL) {
    lslogging_log_message( "lspmac_flight_lut_setup: out of memory");
    exit( -1);
  }

  for( i=0, dp = flight->lut; i<flight->nlut; i++, dp += 2) {
    p = lsredis_get_obj( "cam.zoom.%d.FrontLightIntensity", i);
    if( p==NULL || strlen( lsredis_getstr(p)) == 0) {
      free( flight->lut);
      flight->nlut = 0;
      pthread_mutex_unlock( &flight->mutex);
      lslogging_log_message( "lspmac_flight_lut_setup: cannot find MotorPosition element for cam.flight level %d", i);
      return;
    }
    *dp = i;
    *(dp+1) = 32767.0 * lsredis_getd( p) / 100.0;
  }
  pthread_mutex_unlock( &flight->mutex);
}

/** Set up lookup table for blight
 */
void lspmac_blight_lut_setup() {
  int i;
  lsredis_obj_t *p;
  double *dp;

  pthread_mutex_lock( &blight->mutex);

  blight->nlut = 11;
  blight->lut = calloc( 2 * blight->nlut, sizeof( double));
  if( blight->lut == NULL) {
    lslogging_log_message( "lspmac_blight_lut_setup: out of memory");
    exit( -1);
  }

  for( i=0, dp = blight->lut; i<blight->nlut; i++, dp += 2) {
    p = lsredis_get_obj( "cam.zoom.%d.LightIntensity", i);
    if( p==NULL || strlen( lsredis_getstr(p)) == 0) {
      free( blight->lut);
      blight->nlut = 0;
      pthread_mutex_unlock( &blight->mutex);
      lslogging_log_message( "lspmac_blight_lut_setup: cannot find MotorPosition element for cam.blight level %d", i);
      return;
    }
    *dp = i;
    *(dp+1) = 20000.0 * lsredis_getd( p) / 100.0;
  }
  pthread_mutex_unlock( &blight->mutex);
}

/** Set up lookup table for fscint
 */
void lspmac_fscint_lut_setup() {
  int i;
  double *dp;

  pthread_mutex_lock( &fscint->mutex);

  fscint->nlut = 101;
  fscint->lut = calloc( 2 * fscint->nlut, sizeof( double));
  if( fscint->lut == NULL) {
    lslogging_log_message( "lspmac_fscint_lut_setup: out of memory");
    exit( -1);
  }

  for( i=0, dp = fscint->lut; i<fscint->nlut; i++, dp += 2) {
    *(dp)   = i;
    *(dp+1) = 320.0 * i;
  }
  pthread_mutex_unlock( &fscint->mutex);
}


/** Start up the lspmac thread
 */
void lspmac_run() {
  char **inits;
  lspmac_motor_t *mp;
  int i;
  int active;

  pthread_create( &pmac_thread, NULL, lspmac_worker, NULL);

  lsevents_add_listener( "CryoSwitchChanged",    lspmac_cryoSwitchChanged_cb);
  lsevents_add_listener( "scint In Position",    lspmac_scint_inPosition_cb);
  lsevents_add_listener( "scintDried",           lspmac_scint_dried_cb);
  lsevents_add_listener( "backLight 1",	         lspmac_backLight_up_cb);
  lsevents_add_listener( "backLight 0",	         lspmac_backLight_down_cb);
  lsevents_add_listener( "cam.zoom In Position", lspmac_light_zoom_cb);

  lspmac_zoom_lut_setup();
  lspmac_flight_lut_setup();
  lspmac_blight_lut_setup();
  lspmac_fscint_lut_setup();

  //
  // Initialize the MD2 pmac (ie, turn on the right plcc's etc)
  //
  for( inits = lsredis_get_string_array(lspmac_md2_init); *inits != NULL; inits++) {
    lspmac_SockSendline( *inits);
  }
  
  //
  // Initialize the pmac's support for each motor
  // (ie, set the various flag for when a motor is active or not)
  //
  for( i=0; i<lspmac_nmotors; i++) {
    mp = &(lspmac_motors[i]);
    active = lsredis_getb( mp->active);

    // if there is a problem with "active" then don't do anything
    // On the other hand, various combinations of yes/no true/fals 1/0 should work
    //
    switch( active) {
    case 1:
      inits = lsredis_get_string_array( mp->active_init);
      break;

    case 0:
      inits = lsredis_get_string_array( mp->active_init);
      break;

    default:
      inits = NULL;
    }

    if( inits != NULL) {
      while( *inits != NULL) {
	lspmac_SockSendline( *inits);
	inits++;
      }
    }
  }
}
