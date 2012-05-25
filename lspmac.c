#include "pgpmac.h"

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

typedef unsigned char BYTE;
typedef unsigned short WORD;


static pthread_t pmac_thread;			// our thread to manage access and communication to the pmac
static pthread_mutex_t pmac_queue_mutex;	// manage access to the pmac command queue
static struct pollfd pmacfd;		        // our poll structure



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


static int linesReceived=0;			// current number of lines received
static unsigned char dbmem[64*1024];		// double buffered memory
static int dbmemIn = 0;				// next location

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
  
} md2_status_t;

static md2_status_t md2_status;

void hex_dump( int n, unsigned char *s) {
  int i,j;

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

void cleanstr( char *s) {
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

void lsConnect( char *ipaddr) {
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

    pthread_mutex_lock( &ncurses_mutex);

    wprintw( term_output, "Could not find address: %s\n", gai_strerror( err));

    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();

    pthread_mutex_unlock( &ncurses_mutex);

    return;
  }

  
  addrP = (struct sockaddr_in *)ai_resultP->ai_addr;
  addrP->sin_port = htons( PMACPORT);


  psock = socket( PF_INET, SOCK_STREAM, 0);
  if( psock == -1) {

    pthread_mutex_lock( &ncurses_mutex);
    wprintw( term_output, "Could not create socket\n");

    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
    pthread_mutex_unlock( &ncurses_mutex);
    return;
  }

  err = connect( psock, (const struct sockaddr *)addrP, sizeof( *addrP));
  if( err != 0) {
    pthread_mutex_lock( &ncurses_mutex);
    wprintw( term_output, "Could not connect socket: %s\n", strerror( errno));

    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
    pthread_mutex_unlock( &ncurses_mutex);
    return;
  }
  
  ls_pmac_state = LS_PMAC_STATE_IDLE;
  pmacfd.fd     = psock;
  pmacfd.events = POLLIN;

}

//
// put a new command on the queue
//
void lsPmacPushQueue( pmac_cmd_queue_t *cmd) {

  pthread_mutex_lock( &pmac_queue_mutex);
  memcpy( &(ethCmdQueue[(ethCmdOn++) % PMAC_CMD_QUEUE_LENGTH]), cmd, sizeof( pmac_cmd_queue_t));
  pthread_mutex_unlock( &pmac_queue_mutex);
}

pmac_cmd_queue_t *lsPmacPopQueue() {
  pmac_cmd_queue_t *rtn;

  pthread_mutex_lock( &pmac_queue_mutex);

  if( ethCmdOn == ethCmdOff)
    rtn = NULL;
  else
    rtn = &(ethCmdQueue[(ethCmdOff++) % PMAC_CMD_QUEUE_LENGTH]);

  pthread_mutex_unlock( &pmac_queue_mutex);
  return rtn;
}


pmac_cmd_queue_t *lsPmacPopReply() {
  pmac_cmd_queue_t *rtn;

  pthread_mutex_lock( &pmac_queue_mutex);

  if( ethCmdOn == ethCmdReply)
    rtn = NULL;
  else
    rtn = &(ethCmdQueue[(ethCmdReply++) % PMAC_CMD_QUEUE_LENGTH]);

  pthread_mutex_unlock( &pmac_queue_mutex);
  return rtn;
}

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
    pthread_mutex_lock( &ncurses_mutex);
    wprintw( term_output, "Message Length %d longer than maximum of %ld, aborting\n", wLength, sizeof( cmd.pcmd.bData));

    wnoutrefresh( term_output);
    wnoutrefresh( term_input);
    doupdate();
    pthread_mutex_unlock( &ncurses_mutex);
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
      pthread_mutex_lock( &ncurses_mutex);
      wprintw( term_output, "\n%s\n", pmac_error_strs[err]);
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      pthread_mutex_unlock( &ncurses_mutex);
    }
  }
  lsPmacReset();
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
      cmd = lsPmacPopQueue();      
      if( cmd != NULL) {
	if( cmd->pcmd.Request == VR_PMAC_GETMEM) {
	  nsent = send( evt->fd, cmd, pmac_cmd_size, 0);
	  if( nsent != pmac_cmd_size) {
	    pthread_mutex_lock( &ncurses_mutex);
	    wprintw( term_output, "\nCould only send %d of %d bytes....Not good.", (int)nsent, (int)(pmac_cmd_size));
	    wnoutrefresh( term_output);
	    wnoutrefresh( term_input);
	    doupdate();
	    pthread_mutex_unlock( &ncurses_mutex);
	  }
	} else {
	  nsent = send( evt->fd, cmd, pmac_cmd_size + ntohs(cmd->pcmd.wLength), 0);
	  gettimeofday( &pmac_time_sent, NULL);
	  if( nsent != pmac_cmd_size + ntohs(cmd->pcmd.wLength)) {
	    pthread_mutex_lock( &ncurses_mutex);
	    wprintw( term_output, "\nCould only send %d of %d bytes....Not good.", (int)nsent, (int)(pmac_cmd_size + ntohs(cmd->pcmd.wLength)));
	    wnoutrefresh( term_output);
	    wnoutrefresh( term_input);
	    doupdate();
	    pthread_mutex_unlock( &ncurses_mutex);
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
	pthread_mutex_lock( &ncurses_mutex);
	wprintw( term_output, "\nOut of memory\n");
	wnoutrefresh( term_output);
	wnoutrefresh( term_input);
	doupdate();
	pthread_mutex_unlock( &ncurses_mutex);
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



void PmacGetShortReplyCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
  char *sp;

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

void PmacSendControlReplyPrintCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
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
  static char s[256];
  char *sp;
  int i, pos;
  lspmac_motor_t *dtp;

  memcpy( &md2_status, buff, sizeof(md2_status));


  pthread_mutex_lock( &ncurses_mutex);

  for( i=0; i<lspmac_nmotors; i++) {
    dtp = &(lspmac_motors[i]);
    memcpy( &(dtp->raw_position), buff+dtp->dpram_position_offset, sizeof( int));
    memcpy( &(dtp->status1), buff+dtp->status1_offset, sizeof( int));
    memcpy( &(dtp->status2), buff+dtp->status2_offset, sizeof( int));

    mvwprintw( dtp->win, 2, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, " ");
    mvwprintw( dtp->win, 2, 1, "%*d cts", LS_DISPLAY_WINDOW_WIDTH-6, dtp->raw_position);
    mvwprintw( dtp->win, 3, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, " ");

    snprintf( s, sizeof(s)-1, dtp->format, 8, dtp->raw_position/dtp->u2c);
    s[sizeof(s)-1] = 0;
    mvwprintw( dtp->win, 3, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-6, s);

    mvwprintw( dtp->win, 4, 1, "%*u", LS_DISPLAY_WINDOW_WIDTH-2, dtp->status1);
    mvwprintw( dtp->win, 5, 1, "%*u", LS_DISPLAY_WINDOW_WIDTH-2, dtp->status2);
    sp = "";
    if( dtp->status2 & 0x000002)
      sp = "Following Warning";
    else if( dtp->status2 & 0x000004)
      sp = "Following Error";
    else if( dtp->status2 & 0x000020)
      sp = "I2T Amp Fault";
    else if( dtp->status2 & 0x000008)
      sp = "Amp. Fault";
    else if( dtp->status2 & 0x000800)
      sp = "Stopped on Limit";
    else if( dtp->status1 & 0x040000)
      sp = "Open Loop";
    else if( ~dtp->status1 & 0x080000)
      sp = "Motor Disabled";
    else if( (dtp->status1 & 0x600000) == 0x600000)
      sp = "Both Limits Tripped";
    else if( dtp->status1 & 0x200000)
      sp = "Positive Limit";
    else if( dtp->status1 & 0x400000)
      sp = "Negative Limit";
    else if( ~dtp->status2 & 0x000400)
      sp = "Not Homed";
    else if( dtp->status2 & 0x000001)
      sp = "In Position";

    mvwprintw( dtp->win, 6, 1, "%*s", LS_DISPLAY_WINDOW_WIDTH-2, sp);
    wnoutrefresh( dtp->win);
  }
  if( md2_status.fs_is_open)
    mvwprintw( term_status, 1, 1, "Shutter Open  ");
  else
    mvwprintw( term_status, 1, 1, "Shutter Closed");
  // acc11c_1
  // mask  bit
  // 0x01  0	Air pressure OK
  // 0x02  1	Air bearing OK
  // 0x04  2	Cryo switch
  // 0x08  3	
  // 0x10  4	
  // 0x20  5	
  // 0x40  6	Cryo is back

  if( md2_status.acc11c_1 & 0x40)
    mvwprintw( term_status, 3, 1, "%*s", -8, "Cryo Out");
  else
    mvwprintw( term_status, 3, 1, "%*s", -8, "Cryo In ");

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
    mvwprintw( term_status, 3, 10, "%*s", -8, "Fluor Out");
  else
    mvwprintw( term_status, 3, 10, "%*s", -8, "Fluor In");

  if( md2_status.acc11c_2 & 0x02)
    mvwprintw( term_status, 2, 1, "%*s", -(LS_DISPLAY_WINDOW_WIDTH-2), "Cap Dectected");
  else
    mvwprintw( term_status, 2, 1, "%*s", -(LS_DISPLAY_WINDOW_WIDTH-2), "Cap Not Dectected");


  // acc11c_3
  // mask  bit
  // 0x01  0	Minikappa OK
  // 0x02  1
  // 0x04  2
  // 0x08  3	Arm Parked

  // acc11c_5
  // mask  bit
  //

  mvwprintw( term_status, 4, 1, "Front: %*d", LS_DISPLAY_WINDOW_WIDTH-2-8, md2_status.front_dac);
  mvwprintw( term_status, 5, 1, "Back: %*d", LS_DISPLAY_WINDOW_WIDTH-2-7, md2_status.back_dac);
  mvwprintw( term_status, 6, 1, "Piezo: %*d", LS_DISPLAY_WINDOW_WIDTH-2-8, md2_status.scint_piezo);
  wnoutrefresh( term_status);

  wnoutrefresh( term_input);
  doupdate();
  pthread_mutex_unlock( &ncurses_mutex);
}

void PmacGetMd2Status() {
  lsSendCmd( VR_UPLOAD, VR_PMAC_GETMEM, 0x400, 0, sizeof(md2_status_t), NULL, MD2GetStatusCB, 0, -1);
}


void PmacGetAllIVarsCB( pmac_cmd_queue_t *cmd, int nreceived, unsigned char *buff) {
  static char qs[LS_PG_QUERY_STRING_LENGTH];
  char *sp;
  int i;
  for( i=0, sp=strtok(buff, "\r"); sp != NULL; sp=strtok( NULL, "\r"), i++) {
    snprintf( qs, sizeof( qs)-1, "SELECT pmac.md2_ivar_set( %d, '%s')", i, sp);
    qs[sizeof( qs)-1]=0;
    lspg_query_push( NULL, qs);
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
    lspg_query_push( NULL, qs);
  }
  PmacGetAllIVars();
}

void PmacGetAllMVars() {
  static char *cmds = "M0..8191->";
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen( cmds), cmds, PmacGetAllMVarsCB, 0, -1);
}



void lspmac_sendcmd_nocb( char *fmt, ...) {
  static char tmps[1024];
  va_list arg_ptr;

  va_start( arg_ptr, fmt);
  vsnprintf( tmps, sizeof(tmps)-1, fmt, arg_ptr);
  tmps[sizeof(tmps)-1]=0;
  va_end( arg_ptr);

  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDLINE, 0, 0, strlen(tmps), tmps, NULL, 0, -1);
}




void lspmac_next_state() {

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
      pmacfd.events = 0;
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
      pmacfd.events = POLLIN;
      break;
	
    case LS_PMAC_STATE_SC:
    case LS_PMAC_STATE_CR:
    case LS_PMAC_STATE_RR:
    case LS_PMAC_STATE_GB:
      gettimeofday( &now, NULL);
      if(  ((now.tv_sec * 1000000. + now.tv_usec) - (pmac_time_sent.tv_sec * 1000000. + pmac_time_sent.tv_usec)) < PMAC_MIN_CMD_TIME) {
	pmacfd.events = 0;
      } else {
	pmacfd.events = POLLOUT;
      }
      break;
    }
}


void *lspmac_worker( void *dummy) {

  while( 1) {
    int pollrtn;

    lspmac_next_state();

    if( pmacfd.fd == -1) {
      sleep( 10);	// The pmac is not connected.  Should we warn someone?
      continue;
    }

    pollrtn = poll( &pmacfd, 1, 10);
    if( pollrtn) {
      lsPmacService( &pmacfd);
    }
  }
}


void lspmac_init() {
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

  pthread_mutex_init( &pmac_queue_mutex, NULL);
  pmacfd.fd = -1;

}

void lspmac_run() {

  pthread_create( &pmac_thread, NULL, lspmac_worker, NULL);

}
