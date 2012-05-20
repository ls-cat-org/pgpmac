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

#include "pgpmac.h"


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







ls_display_t ls_displays[32];
int ls_ndisplays = 0;
WINDOW *term_output;		// place to print stuff out
WINDOW *term_input;		// place to put the cursor

static int front_dac;
static int back_dac;
static int scint_piezo;

WINDOW *term_status;		// shutter, lamp, air, etc status



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
static struct pollfd pgfda;			// pollfd object for the database connection
static struct pollfd stdinfda;			// Handle input from the keyboard

static PGconn *q = NULL;
static PostgresPollingStatusType lspg_connectPoll_response;
static PostgresPollingStatusType lspg_resetPoll_response;


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

void ls_display_init( ls_display_t *d, int motor_number, int wy, int wx, int dpoff, int dpoffs1, int dpoffs2, char *wtitle) {
  ls_ndisplays++;
  d->dpram_position_offset = dpoff;
  d->status1_offset = dpoffs1;
  d->status2_offset = dpoffs2;
  d->motor_num = motor_number;
  d->win = newwin( LS_DISPLAY_WINDOW_HEIGHT, LS_DISPLAY_WINDOW_WIDTH, wy*LS_DISPLAY_WINDOW_HEIGHT, wx*LS_DISPLAY_WINDOW_WIDTH);
  box( d->win, 0, 0);
  mvwprintw( d->win, 1, 1, "%s", wtitle);
  wnoutrefresh( d->win);
}

int main( int argc, char **argv) {
  static nfds_t nfds;
  static struct pollfd main_pmac_fda;
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

  ls_display_init( &(ls_displays[ 0]),  1, 0, 0, 0x084, 0x004, 0x044, "Omega   #1 &1 X"); 
  ls_display_init( &(ls_displays[ 1]),  2, 0, 1, 0x088, 0x008, 0x048, "Align X #2 &3 X"); 
  ls_display_init( &(ls_displays[ 2]),  3, 0, 2, 0x08C, 0x00C, 0x04C, "Align Y #3 &3 Y"); 
  ls_display_init( &(ls_displays[ 3]),  4, 0, 3, 0x090, 0x010, 0x050, "Align Z #4 &3 Z"); 
  ls_display_init( &(ls_displays[ 4]),  5, 1, 0, 0x094, 0x014, 0x054, "Anal    #5"); 
  ls_display_init( &(ls_displays[ 5]),  6, 1, 1, 0x098, 0x018, 0x058, "Zoom    #6 &4 Z"); 
  ls_display_init( &(ls_displays[ 6]),  7, 1, 2, 0x09C, 0x01C, 0x05C, "Aper Y  #7 &5 Y"); 
  ls_display_init( &(ls_displays[ 7]),  8, 1, 3, 0x0A0, 0x020, 0x060, "Aper Z  #8 &5 Z"); 
  ls_display_init( &(ls_displays[ 8]),  9, 2, 0, 0x0A4, 0x024, 0x064, "Cap Y   #9 &5 U"); 
  ls_display_init( &(ls_displays[ 9]), 10, 2, 1, 0x0A8, 0x028, 0x068, "Cap Z  #10 &5 V"); 
  ls_display_init( &(ls_displays[10]), 11, 2, 2, 0x0AC, 0x02C, 0x06C, "Scin Z #11 &5 W"); 
  ls_display_init( &(ls_displays[11]), 17, 2, 3, 0x0B0, 0x030, 0x070, "Cen X  #17 &2 X"); 
  ls_display_init( &(ls_displays[12]), 18, 3, 0, 0x0B4, 0x034, 0x074, "Cen Y  #18 &2 Y"); 
  ls_display_init( &(ls_displays[13]), 19, 3, 1, 0x0B8, 0x038, 0x078, "Kappa  #19 &7 X"); 
  ls_display_init( &(ls_displays[14]), 20, 3, 2, 0x0BC, 0x03C, 0x07C, "Phi    #20 &7 Y"); 

  term_status = newwin( LS_DISPLAY_WINDOW_HEIGHT, LS_DISPLAY_WINDOW_WIDTH, 3*LS_DISPLAY_WINDOW_HEIGHT, 3*LS_DISPLAY_WINDOW_WIDTH);
  box( term_status, 0, 0);
  wnoutrefresh( term_status);
						      
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


  lspmac_init( &main_pmac_fda);

  //
  //  make sure these file descriptors are not legal until they've been conneceted
  //
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


    lspmac_next_state( &main_pmac_fda);


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
    if( main_pmac_fda.fd != -1) {
      memcpy( &(fda[nfd++]), &main_pmac_fda, sizeof( struct pollfd));
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
	if( fda[i].fd == main_pmac_fda.fd) {
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

