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
#define VR_PMAC_RESPONSE	0xc4
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
  int nexpected;				// number of bytes expected in response
  unsigned char rbuff[1400];			// buffer for the returned bytes
  void (*onResponse)(int, unsigned char *);	// function to call when response is received.  args are (int fd, nreturned, buffer)
} pmac_cmd_queue_t;

#define LS_PMAC_STATE_CONNECTING   0
#define LS_PMAC_STATE_CONNECTED    1

static ls_pmac_state=LS_PMAC_STATE_CONNECTING;

//
// global 'cause we want to modify ".events" when we want to send something out
//
static struct pollfd pmacfda;	// pollfd object for the pmac
static int waitingForBytes=0;	// flag: don't send anything if we are waiting for an acknowledgment


#define PMAC_CMD_QUEUE_LENGTH 2048
static pmac_cmd_queue_t ethCmdQueue[PMAC_CMD_QUEUE_LENGTH];
static unsigned int ethCmdOn  = 0;
static unsigned int ethCmdOff = 0;


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
    fprintf( stderr, "Could not find address: %s\n", gai_strerror( err));
    return;
  }

  
  addrP = (struct sockaddr_in *)ai_resultP->ai_addr;
  addrP->sin_port = htons( PMACPORT);


  psock = socket( PF_INET, SOCK_STREAM, 0);
  if( psock == -1) {
    fprintf( stderr, "Could not create socket\n");
    return;
  }

  err = connect( psock, (const struct sockaddr *)addrP, sizeof( *addrP));
  if( err != 0) {
    fprintf( stderr, "Could not connect socket: %s\n", strerror( errno));
    return;
  }
  
  ls_pmac_state = LS_PMAC_STATE_CONNECTED;
  pmacfda.fd     = psock;
  pmacfda.events = POLLIN;

}


void lsPmacPushQueue( pmac_cmd_queue_t *cmd) {
  memcpy( &(ethCmdQueue[(ethCmdOn++) % PMAC_CMD_QUEUE_LENGTH]), cmd, sizeof( pmac_cmd_t));
}

pmac_cmd_queue_t *lsPmacPopQueue() {
  if( ethCmdOn == ethCmdOff)
    return NULL;

  return &(ethCmdQueue[(ethCmdOff++) % PMAC_CMD_QUEUE_LENGTH]);
}


void lsSendCmd( int rqType, int rq, int wValue, int wIndex, int wLength, unsigned char *data, int nexpected, void (*responseCB)(int, unsigned char *)) {
  static pmac_cmd_queue_t cmd;

  cmd.pcmd.RequestType = rqType;
  cmd.pcmd.Request     = rq;
  cmd.pcmd.wValue      = htons(wValue);
  cmd.pcmd.wIndex      = htons(wIndex);
  cmd.pcmd.wLength     = htons(wLength);
  cmd.nexpected        = nexpected;
  cmd.onResponse       = responseCB;

  //
  // Setting the message buff bData requires a bit more care to avoid over filling it
  // or sending garbage in the unused bytes.
  //
  
  if( wLength > sizeof( cmd.pcmd.bData)) {
    //
    // Bad things happen if we do not catch this case.
    //
    fprintf( stderr, "Message Length %d longer than maximum of %ld, aborting\n", wLength, sizeof( cmd.pcmd.bData));
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



void lsPmacService( struct pollfd *evt) {
  pmac_cmd_queue_t *cmd;
  ssize_t nsent;

  if( evt->revents & (POLLERR | POLLHUP | POLLNVAL)) {
    if( pmacfda.fd != -1) {
      close( pmacfda.fd);
      pmacfda.fd = -1;
    }
    ls_pmac_state = LS_PMAC_STATE_CONNECTING;
    return;
  }


  if( evt->revents & POLLOUT) {
    pmacfda.events ^= POLLOUT;

    if( waitingForBytes)
      return;

    cmd = lsPmacPopQueue();
    if( cmd == NULL)
      return;

    waitingForBytes = 1;
    nsent = send( pmacfda.fd, cmd, pmac_cmd_size + cmd->pcmd.wLength, 0);
    if( nsent != pmac_cmd_size + cmd->pcmd.wLength) {
      fprintf( stderr, "Could only send %d of %d bytes....Not good.", (int)nsent, (int)(pmac_cmd_size + cmd->pcmd.wLength));
    }
    

  }

  if( evt->revents & POLLIN) {
  }
}

void PmacDummyCB( int nreceived, unsigned char *buff) {
  waitingForBytes = 0;
}

void PmacSockFlush() {
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_FLUSH, 0, 0, 0, NULL, 1, PmacDummyCB);
}

void PmacSockSendline( char *s) {
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_GETLINE, 0, 0, strlen( s), s, 1, PmacDummyCB);
}

void PmacSockSendControlChar( char c) {
  lsSendCmd( VR_DOWNLOAD, VR_PMAC_SENDCTRLCHAR, c, 0, 0, NULL, 1, PmacDummyCB);
}


int main( int argc, char **argv) {
  static nfds_t nfds;
  static struct pollfd fda[2];	// input for poll: room for postgres and pmac
  static int nfd = 0;		// number of items in fda
  static int pollrtn = 0;

  while( 1) {
    if( ls_pmac_state == LS_PMAC_STATE_CONNECTING) {
      lsConnect( "192.6.94.5");
      if( ls_pmac_state == LS_PMAC_STATE_CONNECTED)
	PmacSockFlush();
    }

    nfd = 0;
    if( pmacfda.fd != -1) {
      memcpy( &(fda[nfd++]), &pmacfda, sizeof( struct pollfd));
    }
    //
    // The pg pollfd stuff goes here
    //

    if( nfd > 0) {
      pollrtn = poll( fda, nfd, -1);
    } else {
      //
      // No connectons yet.  Wait a bit and try again.
      //
      sleep( 10);
    }
  }
}
