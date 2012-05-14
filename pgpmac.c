//
// pgpmac.c
//
// Some pmac defines, typedefs, functions suggested by Delta Tau
// Accessory 54E User Manual, October 23, 2003 (C) 2003 by Delta Tau Data
// Systems, Inc.  All rights reserved.
//
//
// Original work Copyright (C) 2012 by Keith Brister, Northwestern University, All rights reserved.
//
//
// TODO's:
//	This code is in desperate need of refactoring
//		consider 4 support modules for pmac, pg, ncurses, and epics
//		leaving main with just the event loop.
//
//	Epics support could come by adapting the "e.c" code to work here directly
//	or could come by making use of the existing kv pair mechanism already in place
//	or, as is most likely, a combination of the two.
//
//	Ncurses support could include input lines for SQL queries and direct commands
//	for supporting homing etc.  Perhaps the F keys could change modes or use of
//	special mode changing text commands.  Output is not asynchronous.  Although this is
//	unlikely to cause a problem I'd hate to have the program hang because terminal
//	output is hung up.
//
//	Currently the effort has been to make no changes to the PMAC motion programs or PLC/PLCC
//	programs to maintian 100% compatability with the MD2 VB code.
//	However, it seems that these contain a considerable amount of garbage code left over from
//	earier developments and false starts.  For example, unit conversions should be done at the PMAC level
//	instead of at a higher level and better use of DPRAM to transfer status information.
//
//	PG queries come back as text instead of binary.  We could reduce the numeric errors by
//	using binary and things would run a tad faster, though it is unlikely anyone would
//	notice or care about the speed.  We need to implement support for all the calls
//	currently implemented by Max's C# code.
//
//

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

/*
  This is a state machine (surprise!)
  Lacking is support for writingbuffer, control writing and reading, as well as double buffered memory
  It looks like several different methods of managing PMAC communications are possible.  Here is set up a queue
  of outgoing commands and deal completely with the result before sending the next.  A full handshake of acknowledgements
  and "readready" is expected.

  State		Description

 -1		Reset the connection
  0		Detached: need to connect to tcp port
  1		Idle (waiting for a command to send to the pmac)
  2		Send command
  3		Waiting for command acknowledgement (no further response expected)
  4		Waiting for control character acknowledgement (further response expected)
  5		Waiting for command acknowledgement (further response expected)
  6		Waiting for get memory response
  7		Send controlresponse
  8		Send readready
  9		Waiting for acknowledgement of "readready"
 10		Send readbuffer
 11		Waiting for control response
 12		Waiting for readbuffer response
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
static int ls_pmac_state = LS_PMAC_STATE_DETACHED;

/*
  Database state machine

State		Description

 -4		Initiate connection
 -3		Poll until connection initialization is complete
 -2		Initiate reset
 -1		Poll until connection reset is complete
  1		Idle (wait for a notify from the server)
  2		Send a query to the server
  3		Continue flushing a command to the server
  4		Waiting for a reply
  5		Continue waiting for a reply

*/

#define LS_PG_STATE_INIT	-4
#define LS_PG_STATE_INIT_POLL	-3
#define LS_PG_STATE_RESET	-2
#define LS_PG_STATE_RESET_POLL	-1
#define LS_PG_STATE_IDLE	1
#define LS_PG_STATE_SEND	2
#define LS_PG_STATE_SEND_FLUSH	3
#define LS_PG_STATE_RECV	4

static int ls_pg_state = LS_PG_STATE_INIT;



typedef unsigned char BYTE;
typedef unsigned short WORD;



//
// Stylistically this is a define.  Really, though, this value will never change.
//
#define PMACPORT 1025

//
// this size does not include the data
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


typedef struct lsDisplayStruct {
  int raw_position;		// raw position read from dpram
  int dpram_position_offset;	// offset of our position in the md2_status buffer ready by getmem
  int motor_num;		// pmac motor number
  char *units;			// string to use as the units
  char *format;			// printf format
  double u2c;			// conversion from counts to units
  WINDOW *win;			// our ncurses window
} ls_display_t;
static ls_display_t ls_displays[32];
static ls_ndisplays = 0;
static WINDOW *term_output;		// place to print stuff out
static WINDOW *term_input;		// place to put the cursor

#define LS_DISPLAY_WINDOW_HEIGHT 6
#define LS_DISPLAY_WINDOW_WIDTH  16

typedef struct tagEthernetCmd {
  BYTE RequestType;
  BYTE Request;
  WORD wValue;
  WORD wIndex;
  WORD wLength;
  BYTE bData[1492];
} pmac_cmd_t;

typedef struct pmacCmdQueueStruct {
  pmac_cmd_t pcmd;				// the pmac command to send
  int no_reply;					// 1 = no reply is expected, 0 = expect a reply
  int motor_group;				// group of 8 motors that status has been requested (0-3 for motors 1-8, 9-16, 17-24, and 25-32), -1 means status not yet requested
  unsigned char rbuff[1400];			// buffer for the returned bytes
  void (*onResponse)(struct pmacCmdQueueStruct *,int, unsigned char *);	// function to call when response is received.  args are (int fd, nreturned, buffer)
} pmac_cmd_queue_t;

typedef struct pmacMotorStruct {
  int status;			// 24 bits of status
  double p;			// position
  double v;			// velocity
  double f;			// following error
} pmac_motor_t;


#define LS_PG_QUERY_STRING_LENGTH 1024
typedef struct lspgQueryQueueStruct {
  char qs[LS_PG_QUERY_STRING_LENGTH];				// our queries should all be pretty short as we'll just be calling functions: fixed length here simplifies memory management
  void (*onResponse)( struct lspgQueryQueueStruct *qq, PGresult *pgr);		//
} lspg_query_queue_t;

#define LS_PG_QUERY_QUEUE_LENGTH 16318
static lspg_query_queue_t lspg_query_queue[LS_PG_QUERY_QUEUE_LENGTH];
static unsigned int lspg_query_queue_on    = 0;
static unsigned int lspg_query_queue_off   = 0;
static unsigned int lspg_query_queue_reply = 0;

//
// globals
//
static struct pollfd pmacfda;			// pollfd object for the pmac
static struct pollfd pgfda;			// pollfd object for the database connection
static struct pollfd stdinfda;			// Handle input from the keyboard
static int chatty=0;				// say what is going on
static int linesReceived=0;			// current number of lines received
static pmac_motor_t motors[32];			// current status of our motors
static unsigned char dbmem[64*1024];		// double buffered memory
static int dbmemIn = 0;				// next location
static PGconn *q = NULL;
static PostgresPollingStatusType lspg_connectPoll_response;
static PostgresPollingStatusType lspg_resetPoll_response;

void PmacSockSendline( char *s);

//
// minimum time between commands to the pmac
//
#define PMAC_MIN_CMD_TIME 20000.0
static struct timeval pmac_time_sent, now;	// used to ensure we do not send commands to the pmac too often.  Only needed for non-DB commands.

#define PMAC_CMD_QUEUE_LENGTH 2048
static pmac_cmd_t rr_cmd, gb_cmd, cr_cmd;		// commands to send out "readready", "getbuffer", controlresponse (initialized in main)
static pmac_cmd_queue_t ethCmdQueue[PMAC_CMD_QUEUE_LENGTH];
static unsigned int ethCmdOn    = 0;
static unsigned int ethCmdOff   = 0;
static unsigned int ethCmdReply = 0;

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

/*
**
** MD2 Motors and Coordinate Systems
**

  CS       Motor

  1		1	X = Omega

  2		17	X = Center X
		18	Y = Center Y

  3		2	X = Alignment X
		3	Y = Alignment Y
		4	Z = Alignment Z

  --            5       Analyzer

  4		6	X = Zoom

  5		7	Y = Aperture Y
		8	Z = Aperture Z
		9	U = Capillary Y
	       10	V = Capillary Z
	       11	W = Scintillator Z

  6			(None)

  7	       19	X = Kappa
	       20	Y = Phi

*/

/*
**
** MD2 Motion Programs
**

before calling, set
   M4XX = 1:  flag to indicate we are running program XX
   P variables as arguments

Program		Description
  1		home omega
  2		home alignment table X
  3		home alignment table Y
  4		home alignment table Z
  6		home camera zoom
  7		home aperture Y
  8		home aperture Z
  9		home capillary Y
 10		home capillary Z
 11		home scintillator Z
 17		home center X
 18		home center Y
 19		home kappa
 20		home phi (Home position is not defined for phi ...)
 25		kappa stress test

 26		Combined Incremental move of X and Y in selected coordinate system
			(Does not reset M426)
			P170  = X increment
			P171  = Y increment

 31		scan omega
			P170  = Start
			P171  = End
			P173  = Velocity (float)
			P174  = Sample Rate (I5049)
			P175  = Acceleration time
			P176  = Gathering source
			P177  = Number of passes
			P178  = Shutter rising distance (units of omega motion)
			P179  = Shutter falling distance (units of omega motion)
			P180  = Exposure Time

 34		Organ Scan
			P169  = Motor Number
			P170  = Start Position
			P171  = End Position
			P172  = Step Size
			P173  = Motor Speed

 35		Organ Homing

 37		Organ Move   (microdiff_hard.ini says we don't use this anymore)
			P169  = Capillary Z
			P170  = Scintillator Z
			P171  = Aperture Z

 50		Combined Incremental move of X and Y
			P170  = X increment
			P171  = Y increment

 52		X oscillation (while M320 == 1)
			(Does not reset M452)
		    
 53		Center X and Y Synchronized homing

 54		Combined X, Y, Z absolute move
			P170  = X
			P171  = Y
			P172  = Z


*/


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
  int omegaHome;		// 0x00           $60060 omega encoder reading at home position
  int dummy1[31];		//
  int omega;			// 0x080           $60080
  int tablex;			// 0x084           $60081
  int tabley;			// 0x088           $60082
  int tablez;			// 0x08C           $60083
  int analyzer;			// 0x090           $60084
  int zoom;			// 0x094           $60085
  int aperturey;		// 0x098           $60086
  int aperturez;		// 0x09C           $60087
  int capy;			// 0x0A0           $60088
  int capz;			// 0x0A4           $60089
  int scinz;			// 0x0A8           $6008A
  int dummy2[5];		// 0x0AC - 0x0BC   $6008B - $6008F
  int centerx;			// 0x0C0           $60090
  int centery;			// 0x0C4           $60091
  int kappa;			// 0x0C8           $60092
  int phi;			// 0x0CC           $60093
  int dummy3[4];		// 0x0D0 - 0x0DC   $60094 - $60097
  int dummy4[28];		// 0x0E0 - 0x14C   $60098 - $600B3
  int freq;			// 0x150           $600B4  (commented out in Pmac_MD2.pmc so it's not being set)
  int dummy5[11];		// 0x154 - 0x17C   $600B5 - $600BF
  int globalstat1;		// 0x180           $600C0
  int globalstat2;		// 0x184           $600C1
  int globalstat3;		// 0x188           $600C2

} md2_status_t;

static md2_status_t md2_status;


// swap double to put in into network byte order                                                                                                                           
// from http://www.dmh2000.com/cpp/dswap.shtml                                                                                                                             
//                                                                                                                                                                         
unsigned long long  swapd(double d) {
  unsigned long long a;
  unsigned char *dst = (unsigned char *)&a;
  unsigned char *src = (unsigned char *)&d;

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];

  return a;
}

double unswapd( long long a) {
  double d;
  unsigned char *dst = (unsigned char *)&d;
  unsigned char *src = (unsigned char *)&a;

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];

  return d;
}


void hex_dump( int n, unsigned char *s) {
  int i,j;
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
}

void cleanstr( char *s) {
  int i;

  for( i=0; i<strlen( s); i++) {
    if( s[i] == '\r')
      wprintw( term_output, "\n");
    else
      wprintw( term_output, "%c", s[i]);
  }
}

void lsConnect( char *ipaddr) {
  int psock;			// our socket: value stored in pmacfda.fd
  int err;			// error code from some system calls
  struct sockaddr_in *addrP;	// our address structure to connect to
  struct addrinfo ai_hints;	// required for getaddrinfo
  struct addrinfo *ai_resultP;	// linked list of address structures (we'll always pick the first)

  pmacfda.fd     = -1;
  pmacfda.events = 0;

  // Initial buffer(s)
  memset( &ai_hints,  0, sizeof( ai_hints));
  
  ai_hints.ai_family   = AF_INET;
  ai_hints.ai_socktype = SOCK_STREAM;
  

  //
  // get address
  //
  err = getaddrinfo( ipaddr, NULL, &ai_hints, &ai_resultP);
  if( err != 0) {
    wprintw( term_output, "Could not find address: %s\n", gai_strerror( err));

    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
    return;
  }

  
  addrP = (struct sockaddr_in *)ai_resultP->ai_addr;
  addrP->sin_port = htons( PMACPORT);


  psock = socket( PF_INET, SOCK_STREAM, 0);
  if( psock == -1) {
    wprintw( term_output, "Could not create socket\n");

    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
    return;
  }

  err = connect( psock, (const struct sockaddr *)addrP, sizeof( *addrP));
  if( err != 0) {
    wprintw( term_output, "Could not connect socket: %s\n", strerror( errno));

    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
    return;
  }
  
  ls_pmac_state = LS_PMAC_STATE_IDLE;
  pmacfda.fd     = psock;
  pmacfda.events = POLLIN;

}


//
// put a new command on the queue
//
void lsPmacPushQueue( pmac_cmd_queue_t *cmd) {

  memcpy( &(ethCmdQueue[(ethCmdOn++) % PMAC_CMD_QUEUE_LENGTH]), cmd, sizeof( pmac_cmd_queue_t));
}

pmac_cmd_queue_t *lsPmacPopQueue() {
  pmac_cmd_queue_t *rtn;

  if( ethCmdOn == ethCmdOff)
    return NULL;

  rtn = &(ethCmdQueue[(ethCmdOff++) % PMAC_CMD_QUEUE_LENGTH]);

  return rtn;
}


pmac_cmd_queue_t *lsPmacPopReply() {
  if( ethCmdOn == ethCmdReply)
    return NULL;
  
  return &(ethCmdQueue[(ethCmdReply++) % PMAC_CMD_QUEUE_LENGTH]);
}

lspg_query_queue_t *lspg_query_next() {
  if( lspg_query_queue_off == lspg_query_queue_on)
    return NULL;

  return &(lspg_query_queue[(lspg_query_queue_off++) % LS_PG_QUERY_QUEUE_LENGTH]);
}

void lspg_query_reply_next() {
  //
  // this is called only when there is nothing else to do to service
  // the reply: this pop does not return anything.
  //  We use the ...reply_peek function to return the next item in the reply queue
  //

  if( lspg_query_queue_reply != lspg_query_queue_on)
    lspg_query_queue_reply++;
}

lspg_query_queue_t *lspg_query_reply_peek() {
  //
  // Return the next item in the reply queue but don't pop it since we may need it more than once.
  //
  if( lspg_query_queue_reply == lspg_query_queue_on)
    return NULL;

  return &(lspg_query_queue[(lspg_query_queue_reply) % LS_PG_QUERY_QUEUE_LENGTH]);
}

void lspg_query_push( char *s, void (*cb)( lspg_query_queue_t *, PGresult *pgr)) {
  int idx;

  idx = lspg_query_queue_on % LS_PG_QUERY_QUEUE_LENGTH;

  strncpy( lspg_query_queue[idx].qs, s, LS_PG_QUERY_STRING_LENGTH - 1);
  lspg_query_queue[idx].qs[LS_PG_QUERY_STRING_LENGTH - 1] = 0;
  lspg_query_queue[idx].onResponse = cb;
  lspg_query_queue_on++;
};


void lsSendCmd( int rqType, int rq, int wValue, int wIndex, int wLength, unsigned char *data, void (*responseCB)(pmac_cmd_queue_t *, int, unsigned char *), int no_reply, int motor_group) {
  static pmac_cmd_queue_t cmd;

  cmd.pcmd.RequestType = rqType;
  cmd.pcmd.Request     = rq;
  cmd.pcmd.wValue      = htons(wValue);
  cmd.pcmd.wIndex      = htons(wIndex);
  cmd.pcmd.wLength     = htons(wLength);
  cmd.onResponse       = responseCB;
  cmd.no_reply	       = no_reply;
  cmd.motor_group      = motor_group;

  //
  // Setting the message buff bData requires a bit more care to avoid over filling it
  // or sending garbage in the unused bytes.
  //
  
  if( wLength > sizeof( cmd.pcmd.bData)) {
    //
    // Bad things happen if we do not catch this case.
    //
    wprintw( term_output, "Message Length %d longer than maximum of %ld, aborting\n", wLength, sizeof( cmd.pcmd.bData));

    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
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

  lsPmacPushQueue( &cmd);
}


void PmacSockFlush() {
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_FLUSH, 0, 0, 0, NULL, NULL, 1, -1);
}

void lsPmacReset() {
  ls_pmac_state = LS_PMAC_STATE_IDLE;

  // clear queue
  ethCmdReply = ethCmdOn;
  ethCmdOff   = ethCmdOn;

  PmacSockFlush();
}


//
// the service routing detected an error condition
//
void lsPmacError( unsigned char *buff) {
  int err;
  //
  // assume buff points to a 1400 byte array of stuff read from the pmac
  //

  if( buff[0] == 7 && buff[1] == 'E' && buff[2] == 'R' && buff[3] == 'R') {
    buff[7] = 0;  // For null termination
    err = atoi( &(buff[4]));
    if( err > 0 && err < 20) {
      wprintw( term_output, "\n%s\n", pmac_error_strs[err]);
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
    }
  }
  lsPmacReset();
}


void lspg_init_motors_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  int i, j;
  uint32_t  motor_number, motor_number_column;
  uint32_t units_column;
  uint32_t u2c_column;
  uint32_t format_column;
  char *sp;
  ls_display_t *lsdp;
  
  motor_number_column = PQfnumber( pgr, "mm_motor");
  units_column        = PQfnumber( pgr, "mm_unit");
  u2c_column          = PQfnumber( pgr, "mm_u2c");
  format_column       = PQfnumber( pgr, "mm_printf");

  if( motor_number_column == -1 || units_column == -1 || u2c_column == -1 || format_column == -1)
    return;

  for( i=0; i<PQntuples( pgr); i++) {

    motor_number = atoi(PQgetvalue( pgr, i, motor_number_column));

    lsdp = NULL;
    for( j=0; j<ls_ndisplays; j++) {
      if( ls_displays[j].motor_num == motor_number) {
	lsdp = &(ls_displays[j]);
	lsdp->units = strdup( PQgetvalue( pgr, i, units_column));
	lsdp->format= strdup( PQgetvalue( pgr, i, format_column));
	lsdp->u2c   = atof(PQgetvalue( pgr, i, u2c_column));
	break;
      }
    }
    if( lsdp == NULL)
      continue;
      

    if( fabs(lsdp->u2c) <= 1.0e-9)
      lsdp->u2c = 1.0;
      
  }
}

void lspg_cmd_cb( lspg_query_queue_t *qqp, PGresult *pgr) {
  //
  // Call back funciton assumes query results in zero or more commands to send to the PMAC
  //
  int i;
  char *sp;
  
  for( i=0; i<PQntuples( pgr); i++) {
    sp = PQgetvalue( pgr, i, 0);
    if( sp != NULL && *sp != 0) {
      PmacSockSendline( sp);
      //
      // Keep asking for more until
      // there are no commands left
      // 
      // This should solve a potential problem where
      // more than one command is put on the queue for a given notify.
      //
      lspg_query_push( "select pmac.md2_queue_next()", lspg_cmd_cb);
    }
  }
}

void lsPGService( struct pollfd *evt) {
  //
  // Currently just used to check for notifies
  // Other socket communication is done syncronously
  // Reconsider this if we start using the pmac gather functions
  // since we'll want to be servicing those sockets ASAP
  //

  if( evt->revents & POLLIN) {
    int err;

    if( ls_pg_state == LS_PG_STATE_INIT_POLL) {
      lspg_connectPoll_response = PQconnectPoll( q);
      if( lspg_connectPoll_response == PGRES_POLLING_FAILED) {
	ls_pg_state = LS_PG_STATE_RESET;
      }
      return;
    }

    if( ls_pg_state == LS_PG_STATE_RESET_POLL) {
      lspg_resetPoll_response = PQresetPoll( q);
      if( lspg_resetPoll_response == PGRES_POLLING_FAILED) {
	ls_pg_state = LS_PG_STATE_RESET;
      }
      return;
    }


    //
    // if in IDLE or RECV we need to call consumeInput first
    //
    err = PQconsumeInput( q);
    if( err != 1) {
      wprintw( term_output, "\nconsume input failed: %s\n", PQerrorMessage( q));
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      ls_pg_state == LS_PG_STATE_RESET;
      return;
    }
      

    if( ls_pg_state == LS_PG_STATE_RECV) {
      PGresult *pgr;
      lspg_query_queue_t *qqp;

      //
      // We must call PQgetResult until it returns NULL before sending the next query
      // This implies that only one query can ever be active at a time and our queue
      // management should be simple
      //
      // We should be in the LS_PG_STATE_RECV here
      //

      while( !PQisBusy( q)) {
	pgr = PQgetResult( q);
	if( pgr == NULL) {
	  lspg_query_reply_next();
	  //
	  // we are now done reading the response from the database
	  //
	  ls_pg_state = LS_PG_STATE_IDLE;
	  break;
	} else {
	  ExecStatusType es;

	  qqp = lspg_query_reply_peek();
	  es = PQresultStatus( pgr);

	  if( es != PGRES_COMMAND_OK && es != PGRES_TUPLES_OK) {
	    char *emess;
	    emess = PQresultErrorMessage( pgr);
	    if( emess != NULL && emess[0] != 0) {
	      wprintw( term_output, "\nError from query '%s':\n%s\n", qqp->qs, emess);
	      wnoutrefresh( term_output);
	      wnoutrefresh( term_input);
	      doupdate();
	    }
	  } else {
	    //
	    // Deal with the response
	    //
	    // If the response is likely to take a while we should probably
	    // add a new state and put something in the main look to run the onResponse
	    // routine in the main loop.  For now, though, we only expect very breif onResponse routines
	    //
	    if( qqp != NULL && qqp->onResponse != NULL)
	      qqp->onResponse( qqp, pgr);
	  }
	  PQclear( pgr);
	}
      }
    }

    //
    // Check for notifies regardless of our state
    // Push as many requests as we have notifies.
    //
    {
      PGnotify *pgn;

      while( 1) {
	pgn = PQnotifies( q);
	if( pgn == NULL)
	  break;
	lspg_query_push( "select pmac.md2_queue_next()", lspg_cmd_cb);
	PQfreemem( pgn);
      }
    }
  }

  if( evt->revents & POLLOUT) {

    if( ls_pg_state == LS_PG_STATE_INIT_POLL) {
      lspg_connectPoll_response = PQconnectPoll( q);
      if( lspg_connectPoll_response == PGRES_POLLING_FAILED) {
	ls_pg_state = LS_PG_STATE_RESET;
      }
      return;
    }

    if( ls_pg_state == LS_PG_STATE_RESET_POLL) {
      lspg_resetPoll_response = PQresetPoll( q);
      if( lspg_resetPoll_response == PGRES_POLLING_FAILED) {
	ls_pg_state = LS_PG_STATE_RESET;
      }
      return;
    }


    if( ls_pg_state == LS_PG_STATE_SEND) {
      lspg_query_queue_t *qqp;
      int err;

      qqp = lspg_query_next();
      if( qqp == NULL) {
	//
	// A send without a query?  Should never happen.
	// But at least we shouldn't segfault if it does.
	//
	return;
      }

      if( qqp->qs[0] == 0) {
	//
	// Do we really have to check this case?
	// It would only come up if we stupidly pushed a null query string
	// or ran off the end of the list
	//
	wprintw( term_output, "\nPopped empty query string.  Probably bad things are going on.\n");
	wnoutrefresh( term_output);
	wnoutrefresh( term_input);
	doupdate();

	lspg_query_reply_next();
	ls_pg_state = LS_PG_STATE_IDLE;
      } else {
	err = PQsendQuery( q, qqp->qs);
	if( err == 0) {
	  wprintw( term_output, "\nquery failed: %s\n", PQerrorMessage( q));
	  wnoutrefresh( term_output);
	  wnoutrefresh( term_input);
	  doupdate();

	  lspg_query_reply_next();
	  ls_pg_state == LS_PG_STATE_RESET;
	} else {
	  ls_pg_state = LS_PG_STATE_RECV;
	}
      }

      if( ls_pg_state == LS_PG_STATE_SEND_FLUSH) {
	err = PQflush( q);
	switch( err) {
	case -1:
	  // an error occured
	  wprintw( term_output, "\nflush failed: %s\n", PQerrorMessage( q));
	  wnoutrefresh( term_output);
	  wnoutrefresh( term_input);
	  doupdate();

	  ls_pg_state = LS_PG_STATE_IDLE;
	  //
	  // We should probably reset the connection and start from scratch.  Probably the connection died.
	  //
	  break;
	  
	case 0:
	  // goodness and joy.
	  break;

	case 1:
	  // more sending to do
	  ls_pg_state = LS_PG_STATE_SEND_FLUSH;
	  break;
	}
      }
    }
  }
}

void lsPmacService( struct pollfd *evt) {
  static unsigned char *receiveBuffer = NULL;	// the buffer inwhich to stick our incomming characters
  static int receiveBufferSize = 0;		// size of receiveBuffer
  static int receiveBufferIn = 0;		// next location to write to in receiveBuffer
  pmac_cmd_queue_t *cmd;			// maybe the command we are servicing
  ssize_t nsent, nread;				// nbytes dealt with
  int i;					// loop counter
  int foundEOCR;				// end of command response flag

  if( evt->revents & (POLLERR | POLLHUP | POLLNVAL)) {
    if( pmacfda.fd != -1) {
      close( pmacfda.fd);
      pmacfda.fd = -1;
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
      cmd = lsPmacPopQueue();      
      if( cmd != NULL) {
	if( cmd->pcmd.Request == VR_PMAC_GETMEM) {
	  nsent = send( pmacfda.fd, cmd, pmac_cmd_size, 0);
	  if( nsent != pmac_cmd_size) {
	    wprintw( term_output, "\nCould only send %d of %d bytes....Not good.", (int)nsent, (int)(pmac_cmd_size));
	    wnoutrefresh( term_output);
	    wnoutrefresh( term_input);
	    doupdate();
	  }
	} else {
	  nsent = send( pmacfda.fd, cmd, pmac_cmd_size + ntohs(cmd->pcmd.wLength), 0);
	  gettimeofday( &pmac_time_sent, NULL);
	  if( nsent != pmac_cmd_size + ntohs(cmd->pcmd.wLength)) {
	    wprintw( term_output, "\nCould only send %d of %d bytes....Not good.", (int)nsent, (int)(pmac_cmd_size + ntohs(cmd->pcmd.wLength)));
	    wnoutrefresh( term_output);
	    wnoutrefresh( term_input);
	    doupdate();
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
      nsent = send( pmacfda.fd, &cr_cmd, pmac_cmd_size, 0);
      gettimeofday( &pmac_time_sent, NULL);
      ls_pmac_state = LS_PMAC_STATE_WCR;
      break;

    case LS_PMAC_STATE_RR:
      nsent = send( pmacfda.fd, &rr_cmd, pmac_cmd_size, 0);
      gettimeofday( &pmac_time_sent, NULL);
      ls_pmac_state = LS_PMAC_STATE_WACK_RR;
      break;

    case LS_PMAC_STATE_GB:
      nsent = send( pmacfda.fd, &gb_cmd, pmac_cmd_size, 0);
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
	wprintw( term_output, "\nOut of memory\n");
	wnoutrefresh( term_output);
	wnoutrefresh( term_input);
	doupdate();
	exit( -1);
      }
      memcpy( newbuff, receiveBuffer, receiveBufferIn);
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
	  lsPmacError( &(receiveBuffer[i]));
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
      cmd = lsPmacPopReply();
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
      cmd = lsPmacPopReply();
      ls_pmac_state = LS_PMAC_STATE_IDLE;
      break;
      
    case LS_PMAC_STATE_WCR:
      cmd = lsPmacPopReply();
      ls_pmac_state = LS_PMAC_STATE_IDLE;
      break;
    case LS_PMAC_STATE_WGB:
      if( foundEOCR) {
	cmd = lsPmacPopReply();
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

void PmacSetPositions( int motor_group, char *s) {
  int i;
  char *sp;

  sp = strtok( s, " ");
  for( i=motor_group*8; sp != NULL && i<(motor_group + 1)*8; i++) {
    motors[i].p = atof( sp);
  }
}
void PmacSetVelocities( int motor_group, char *s) {
  int i;
  char *sp;

  sp = strtok( s, " ");
  for( i=motor_group*8; sp != NULL && i<(motor_group + 1)*8; i++) {
    motors[i].v = atof( sp);
  }
}

void PmacSetFollowingErrors( int motor_group, char *s) {
  int i;
  char *sp;

  sp = strtok( s, " ");
  for( i=motor_group*8; sp != NULL && i<(motor_group + 1)*8; i++) {
    motors[i].f = atof( sp);
  }
}

void PmacSetStatuses( int motor_group, char *s) {
  int i;
  char *sp;

  sp = strtok( s, " ");
  for( i=motor_group*8; sp != NULL && i<(motor_group + 1)*8; i++) {
    motors[i].status = strtol( sp, NULL,  16);
  }
}

void PmacGetShortReplyCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
  char *sp;

  if( nreceived < 1400)
    buff[nreceived]=0;

  sp = (char *)(cmd->pcmd.bData);
  
  if( *buff == 0) {
    wprintw( term_output, "%s\n", sp);
  } else {
    wprintw( term_output, "%s: ", sp);
    cleanstr( buff);
  }
  wnoutrefresh( term_output);
  wnoutrefresh( term_input);
  doupdate();

  memset( cmd->pcmd.bData, 0, sizeof( cmd->pcmd.bData));
}

void PmacSendControlReplyPrintCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
    wprintw( term_output, "control-%c: ", '@'+ ntohs(cmd->pcmd.wValue));
    hex_dump( nreceived, buff);
    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
}

void PmacSendControlReplyCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {


  if( cmd->motor_group >=0 && cmd->motor_group < 4) {
    switch( ntohs(cmd->pcmd.wValue)) {
    case '\x02':
      PmacSetStatuses( cmd->motor_group, buff);
      break;
    case '\x06':
      PmacSetFollowingErrors( cmd->motor_group, buff);
      break;
    case '\x10':
      PmacSetPositions( cmd->motor_group, buff);
      break;
    case '\x16':
      PmacSetVelocities( cmd->motor_group, buff);
      break;
    default:
      wprintw( term_output, "control-%c: ", '@'+ ntohs(cmd->pcmd.wValue));
      cleanstr( buff);
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
    }
  } else {
    wprintw( term_output, "control-%c: ", '@'+ ntohs(cmd->pcmd.wValue));
    cleanstr( buff);
    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
  }


  memset( cmd->pcmd.bData, 0, sizeof( cmd->pcmd.bData));

}

void PmacGetmemReplyCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
  memcpy( &(dbmem[ntohs(cmd->pcmd.wValue)]), buff, nreceived);

  dbmemIn += nreceived;
  if( dbmemIn >= sizeof( dbmem)) {
    dbmemIn = 0;
  }
}
	       
void PmacSockGetmem( int offset, int nbytes)  {
  lsSendCmd( VR_UPLOAD,   VR_PMAC_GETMEM, offset, 0, nbytes, NULL, PmacGetmemReplyCB, 0, -1);
}
void PmacSockSendline( char *s) {
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( s), s, PmacGetShortReplyCB, 0, -1);
}
void PmacSockSendline_nr( char *s) {
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( s), s, NULL, 1, -1);
}

void PmacSockSendControlChar( char c, int motor_group) {
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDCTRLCHAR, c, 0, 0, NULL, PmacSendControlReplyCB, 0, motor_group);
}

void PmacSockSendControlCharPrint( char c) {
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDCTRLCHAR, c, 0, 0, NULL, PmacSendControlReplyPrintCB, 0, -1);
}

void PmacGetmem() {
  int nbytes;
  nbytes = (dbmemIn + 1400 > sizeof( dbmem)) ? sizeof( dbmem) - dbmemIn : 1400;
  PmacSockGetmem( dbmemIn, nbytes);
}

void MD2GetStatusCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
  static int cnt = 0;
  int i, pos;
  ls_display_t *dtp;

  memcpy( &md2_status, buff, sizeof(md2_status));

  //  hex_dump( nreceived, buff);


  for( i=0; i<ls_ndisplays; i++) {
    dtp = &(ls_displays[i]);
    memcpy( &(dtp->raw_position), buff+dtp->dpram_position_offset, sizeof( int));
    mvwprintw( dtp->win, 2, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, " ");
    mvwprintw( dtp->win, 2, 1, "%*d cts", LS_DISPLAY_WINDOW_WIDTH-6, dtp->raw_position);
    mvwprintw( dtp->win, 3, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, " ");
    mvwprintw( dtp->win, 3, 1, dtp->format, LS_DISPLAY_WINDOW_WIDTH-2-strlen(dtp->units)-1, dtp->raw_position/dtp->u2c);
    wnoutrefresh( dtp->win);
  }
  wnoutrefresh( term_input);
  doupdate();
}

void PmacGetMd2Status() {
  lsSendCmd( VR_UPLOAD, VR_PMAC_GETMEM, 0x180, 0, sizeof(md2_status_t), NULL, MD2GetStatusCB, 0, -1);
}


void PmacGetAllIVarsCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
  static char qs[LS_PG_QUERY_STRING_LENGTH];
  char *sp;
  int i;
  for( i=0, sp=strtok(buff, "\r"); sp != NULL; sp=strtok( NULL, "\r"), i++) {
    snprintf( qs, sizeof( qs)-1, "SELECT pmac.md2_ivar_set( %d, '%s')", i, sp);
    qs[sizeof( qs)-1]=0;
    lspg_query_push( qs, NULL);
  }
}

void PmacGetAllIVars() {
  static char *cmds = "I0..8191";
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( cmds), cmds, PmacGetAllIVarsCB, 0, -1);
}

void PmacGetAllMVarsCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
  static char qs[LS_PG_QUERY_STRING_LENGTH];
  char *sp;
  int i;
  for( i=0, sp=strtok(buff, "\r"); sp != NULL; sp=strtok( NULL, "\r"), i++) {
    snprintf( qs, sizeof( qs)-1, "SELECT pmac.md2_mvar_set( %d, '%s')", i, sp);
    qs[sizeof( qs)-1]=0;
    lspg_query_push( qs, NULL);
  }
  PmacGetAllIVars();
}

void PmacGetAllMVars() {
  static char *cmds = "M0..8191->";
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( cmds), cmds, PmacGetAllMVarsCB, 0, -1);
}


void PmacGetMotors() {
  int i;
  char s[32];
  for( i=0; i<4; i++) {
    sprintf( s, "##%d", i);
    PmacSockSendline_nr( s);
    PmacSockSendControlChar( '\x02', i);	// control-b report 8 motor status words to host
    PmacSockSendControlChar( '\x06', i);	// control-f report 8 motor following errors
    PmacSockSendControlChar( '\x10', i);	// control-p report 8 motor positions
    PmacSockSendControlChar( '\x16', i);	// control-v report 8 motor velocities
  }
}


void pg_conn() {
  PGresult *pgr;
  int wait_interval = 1;
  int connection_init = 0;
  int i, err;

  if( q == NULL)
    ls_pg_state = LS_PG_STATE_INIT;

  switch( ls_pg_state) {
  case LS_PG_STATE_INIT:
    q = PQconnectStart( "dbname=ls user=lsuser hostaddr=10.1.0.3");
    if( q == NULL) {
      wprintw( term_output, "Out of memory (pg_conn)\n");
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      exit( -1);
    }
    err = PQstatus( q);
    if( err == CONNECTION_BAD) {
      wprintw( term_output, "Trouble connecting to database\n");
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      //
      // TODO: save time of day so we can check that we are not retrying the connection too often
      //
      return;
    }
    err = PQsetnonblocking( q, 1);
    if( err != 0) {
      wprintw( term_output, "Odd, could not set database connection to nonblocking\n");
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
    }

    ls_pg_state = LS_PG_STATE_INIT_POLL;
    lspg_connectPoll_response = PGRES_POLLING_WRITING;
    //
    // set up the connection for poll
    //
    pgfda.fd = PQsocket( q);
    break;

  case LS_PG_STATE_INIT_POLL:
    if( lspg_connectPoll_response == PGRES_POLLING_FAILED) {
      PQfinish( q);
      q = NULL;
      ls_pg_state = LS_PG_STATE_INIT;
    } else if( lspg_connectPoll_response == PGRES_POLLING_OK) {
      lspg_query_push( "select * from pmac.md2_getmotors()", lspg_init_motors_cb);
      lspg_query_push( "select pmac.md2_init()", NULL);
      ls_pg_state = LS_PG_STATE_IDLE;
    }
    break;

  case LS_PG_STATE_RESET:
    err = PQresetStart( q);
    if( err == 0) {
      PQfinish( q);
      q = NULL;
      ls_pg_state = LS_PG_STATE_INIT;
    } else {
      ls_pg_state = LS_PG_STATE_RESET_POLL;
      lspg_resetPoll_response = PGRES_POLLING_WRITING;
    }
    break;

  case LS_PG_STATE_RESET_POLL:
    if( lspg_resetPoll_response == PGRES_POLLING_FAILED) {
      PQfinish( q);
      q = NULL;
      ls_pg_state = LS_PG_STATE_INIT;
    } else if( lspg_resetPoll_response == PGRES_POLLING_OK) {
      lspg_query_push( "select * from pmac.md2_getmotors()", lspg_init_motors_cb);
      lspg_query_push( "select pmac.md2_init()", NULL);
      ls_pg_state = LS_PG_STATE_IDLE;
    }
    break;
  }
}


void stdinService( struct pollfd *evt) {
  static char cmds[1024];
  static char cntrlcmd[2];
  static char cmds_on = 0;
  int ch;


  for( ch=wgetch(term_input); ch != ERR; ch=wgetch(term_input)) {
    // wprintw( term_output, "%04x\n", ch);
    // wnoutrefresh( term_output);

    switch( ch) {
    case KEY_F(1):
      endwin();
      exit(0);
      break;

    case 0x0001:	// Control-A
    case 0x0002:	// Control-B
    case 0x0003:	// Control-C
    case 0x0004:	// Control-D
    case 0x0005:	// Control-E
    case 0x0006:	// Control-F
    case 0x0007:	// Control-G
    case 0x000b:	// Control-K
    case 0x000f:	// Control-O
    case 0x0010:	// Control-P
    case 0x0011:	// Control-Q
    case 0x0012:	// Control-R
    case 0x0013:	// Control-Q
    case 0x0016:	// Control-V
      cntrlcmd[0] = ch;
      cntrlcmd[1] = 0;
      PmacSockSendline( cntrlcmd);
      //      PmacSockSendControlCharPrint( ch);
      break;

    case KEY_BACKSPACE:
      cmds[cmds_on] = 0;
      cmds_on == 0 ? 0 : cmds_on--;
      break;
      
    case KEY_ENTER:
    case 0x000a:
      if( cmds_on > 0 && strlen( cmds) > 0) {
	PmacSockSendline( cmds);
      }
      memset( cmds, 0, sizeof(cmds));
      cmds_on = 0;
      break;
      
    default:
      if( cmds_on < sizeof( cmds)-1) {
	cmds[cmds_on++] = ch;
	cmds[cmds_on] = 0;
      }
      break;
    }
    
    mvwprintw( term_input, 1, 1, "PMAC> %s", cmds);
    wclrtoeol( term_input);
    box( term_input, 0, 0);
    wnoutrefresh( term_input);
    doupdate();

  }
}

void ls_display_init( ls_display_t *d, int motor_number, int wy, int wx, int dpoff, char *wtitle) {
  ls_ndisplays++;
  d->dpram_position_offset = dpoff;
  d->motor_num = motor_number;
  d->win = newwin( LS_DISPLAY_WINDOW_HEIGHT, LS_DISPLAY_WINDOW_WIDTH, wy*LS_DISPLAY_WINDOW_HEIGHT, wx*LS_DISPLAY_WINDOW_WIDTH);
  box( d->win, 0, 0);
  mvwprintw( d->win, 1, 1, "%s", wtitle);
  wnoutrefresh( d->win);
}

int main( int argc, char **argv) {
  static nfds_t nfds;
  static struct pollfd fda[3], *fdp;	// input for poll: room for postgres, pmac, and stdin
  static int nfd = 0;			// number of items in fda
  static int pollrtn = 0;
  int i;				// standard loop counter

  stdinfda.fd = 0;
  stdinfda.events = POLLIN;

  initscr();				// Start ncurses
  raw();				// Line buffering disabled, control chars trapped
  keypad( stdscr, TRUE);		// Why is F1 nifty?
  refresh();

  ls_display_init( &(ls_displays[ 0]),  1, 0, 0, 0x080, "Omega   #1 &1"); 
  ls_display_init( &(ls_displays[ 1]),  2, 0, 1, 0x084, "Align X #2 &3"); 
  ls_display_init( &(ls_displays[ 2]),  3, 0, 2, 0x088, "Align Y #3 &3"); 
  ls_display_init( &(ls_displays[ 3]),  4, 0, 3, 0x08C, "Align Z #4 &3"); 
  ls_display_init( &(ls_displays[ 4]),  5, 1, 0, 0x090, "Anal    #5"); 
  ls_display_init( &(ls_displays[ 5]),  6, 1, 1, 0x094, "Zoom    #6 &4"); 
  ls_display_init( &(ls_displays[ 6]),  7, 1, 2, 0x098, "Aper Y  #7 &5"); 
  ls_display_init( &(ls_displays[ 7]),  8, 1, 3, 0x09C, "Aper Z  #8 &5"); 
  ls_display_init( &(ls_displays[ 8]),  9, 2, 0, 0x0A0, "Cap Y   #9 &5"); 
  ls_display_init( &(ls_displays[ 9]), 10, 2, 1, 0x0A4, "Cap Z  #10 &5"); 
  ls_display_init( &(ls_displays[10]), 11, 2, 2, 0x0A8, "Scin Z #11 &5"); 
  ls_display_init( &(ls_displays[11]), 17, 2, 3, 0x0C0, "Cen X  #17 &2"); 
  ls_display_init( &(ls_displays[12]), 18, 3, 0, 0x0C4, "Cen Y  #18 &2"); 
  ls_display_init( &(ls_displays[13]), 19, 3, 1, 0x0C8, "Kappa  #19 &7"); 
  ls_display_init( &(ls_displays[14]), 20, 3, 2, 0x0CC, "Phi    #20 &7"); 


  term_output = newwin( 10, 4*LS_DISPLAY_WINDOW_WIDTH, 4*LS_DISPLAY_WINDOW_HEIGHT, 0);
  scrollok( term_output, 1);
  wnoutrefresh( term_output);

  term_input  = newwin( 3, 4*LS_DISPLAY_WINDOW_WIDTH, 10+4*LS_DISPLAY_WINDOW_HEIGHT, 0);
  box( term_input, 0, 0);
  mvwprintw( term_input, 1, 1, "PMAC> ");
  nodelay( term_input, TRUE);
  keypad( term_input, TRUE);
  wnoutrefresh( term_input);

  doupdate();

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
  //  make sure these file descriptors are not legal until they've been conneceted
  //
  pmacfda.fd = -1;
  pgfda.fd   = -1;

  while( 1) {
    //
    // Big loop: includes initiallizing and reinitiallizing the PMAC
    //

    //
    // connect to the database
    //
    if( q == NULL || ls_pg_state == LS_PG_STATE_INIT || ls_pg_state == LS_PG_STATE_RESET || ls_pg_state == LS_PG_STATE_INIT_POLL || ls_pg_state == LS_PG_STATE_RESET_POLL)
      pg_conn();


    //
    // Connect to the pmac
    //
    if( ls_pmac_state == LS_PMAC_STATE_DETACHED) {
      lsConnect( "192.6.94.5");

      //
      // If the connect was successful we can proceed with the initialization
      //
      if( ls_pmac_state != LS_PMAC_STATE_DETACHED) {
	PmacSockFlush();

	PmacGetAllMVars();
	/*
	PmacSockSendline( "I5=3");
	PmacSockSendline( "ENABLE PLCC 0");
	PmacSockSendline( "ENABLE PLCC 1");
	*/
      }
    }

    //
    // when we are ready, trigger sending a command
    //
    if( ls_pmac_state == LS_PMAC_STATE_IDLE && ethCmdOn != ethCmdOff)
      ls_pmac_state = LS_PMAC_STATE_SC;


    //
    // Set the events flag
    switch( ls_pmac_state) {
    case LS_PMAC_STATE_DETACHED:
      //
      // there shouldn't be a valid fd, so ignore the events
      //
      pmacfda.events = 0;
      break;
	
    case LS_PMAC_STATE_IDLE:
      if( ethCmdOn == ethCmdOff)
	PmacGetMd2Status();

    case LS_PMAC_STATE_WACK_NFR:
    case LS_PMAC_STATE_WACK:
    case LS_PMAC_STATE_WACK_CC:
    case LS_PMAC_STATE_WACK_RR:
    case LS_PMAC_STATE_WCR:
    case LS_PMAC_STATE_WGB:
    case LS_PMAC_STATE_GMR:
      pmacfda.events = POLLIN;
      break;
	
    case LS_PMAC_STATE_SC:
    case LS_PMAC_STATE_CR:
    case LS_PMAC_STATE_RR:
    case LS_PMAC_STATE_GB:
      gettimeofday( &now, NULL);
      if(  ((now.tv_sec * 1000000. + now.tv_usec) - (pmac_time_sent.tv_sec * 1000000. + pmac_time_sent.tv_usec)) < PMAC_MIN_CMD_TIME) {
	pmacfda.events = 0;
      } else {
	pmacfda.events = POLLOUT;
      }
      break;
    }


    if( ls_pg_state == LS_PG_STATE_IDLE && lspg_query_queue_on != lspg_query_queue_off)
      ls_pg_state = LS_PG_STATE_SEND;

    switch( ls_pg_state) {
    case LS_PG_STATE_INIT_POLL:
      if( lspg_connectPoll_response == PGRES_POLLING_WRITING)
	pgfda.events = POLLOUT;
      else if( lspg_connectPoll_response == PGRES_POLLING_READING)
	pgfda.events = POLLIN;
      else
	pgfda.events = 0;
      break;
      
    case LS_PG_STATE_RESET_POLL:
      if( lspg_resetPoll_response == PGRES_POLLING_WRITING)
	pgfda.events = POLLOUT;
      else if( lspg_resetPoll_response == PGRES_POLLING_READING)
	pgfda.events = POLLIN;
      else
	pgfda.events = 0;
      break;

    case LS_PG_STATE_IDLE:
    case LS_PG_STATE_RECV:
      pgfda.events = POLLIN;
      break;

    case LS_PG_STATE_SEND:
    case LS_PG_STATE_SEND_FLUSH:
      pgfda.events = POLLOUT;
      break;

    default:
      pgfda.events = 0;
    }


    nfd = 0;

    //
    // pmac socket
    if( pmacfda.fd != -1) {
      memcpy( &(fda[nfd++]), &pmacfda, sizeof( struct pollfd));
    }
    //
    // postgres socket
    //
    if( pgfda.fd != -1) {
      memcpy( &(fda[nfd++]), &pgfda, sizeof( struct pollfd));
    }
    //
    // keyboard
    //
    memcpy( &(fda[nfd++]), &stdinfda, sizeof( struct pollfd));
    

    if( nfd == 0) {
      //
      // No connectons yet.  Wait a bit and try again.
      //
      sleep( 10);
      //
      // go try to connect again
      //
      continue;
    }


    pollrtn = poll( fda, nfd, 10);

    for( i=0; pollrtn>0 && i<nfd; i++) {
      if( fda[i].revents) {
	pollrtn--;
	if( fda[i].fd == pmacfda.fd) {
	  lsPmacService( &fda[i]);
	} else if( fda[i].fd == PQsocket( q)) {
	  lsPGService( &fda[i]);
	} else if( fda[i].fd == 0) {
	  stdinService( &fda[i]);
	}
      }
    }
  }
}

