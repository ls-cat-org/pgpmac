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
//
//	PG queries come back as text instead of binary.  We could reduce the numeric errors by
//	using binary and things would run a tad faster, though it is unlikely anyone would
//	notice or care about the speed.  We need to support the seqrun code from blumax.
//
//

#include "pgpmac.h"

WINDOW *term_output;			// place to print stuff out
WINDOW *term_input;			// place to put the cursor
WINDOW *term_status;			// shutter, lamp, air, etc status

pthread_mutex_t ncurses_mutex;		// allow more than one thread access to the screen


//
// globals
//
static struct pollfd stdinfda;			// Handle input from the keyboard

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


131		LS-CAT Modified Omega Scan
			P170	= Shutter open position, in counts
			P171    = Delta omega, in counts
			P173    = Omega velocity (counts/msec)
			P175    = Acceleration Time (msec)
			P177    = Number of passes
			P178    = Shutter Rising Distance
			P179    = Shutter Falling Distance
			P180    = Exposure TIme (msec)

140		LS-CAT Move X Absolute
			Q10    = X Value (cts)

141		LS-CAT Move Y Absolute
			Q11    = Y Value (cts)

142		LS-CAT Move Z Absolute
			Q12    = Z Value (cts)
			
150		LS-CAT Move X, Y Absolute
			Q20    = X Value
			Q21    = Y Value

160		LS-CAT Move X, Y, Z  Absolute
			Q30    = X Value
			Q31    = Y Value
			Q32    = Z Value
*/






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
      lspmac_SockSendline( cntrlcmd);
      //      PmacSockSendControlCharPrint( ch);
      break;

    case KEY_BACKSPACE:
      cmds[cmds_on] = 0;
      cmds_on == 0 ? 0 : cmds_on--;
      break;
      
    case KEY_ENTER:
    case 0x000a:
      if( cmds_on > 0 && strlen( cmds) > 0) {
	lspmac_SockSendline( cmds);
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

int main( int argc, char **argv) {
  static nfds_t nfds;

  static struct pollfd fda[3], *fdp;	// input for poll: room for postgres, pmac, and stdin
  static int nfd = 0;			// number of items in fda
  static int pollrtn = 0;
  static struct option long_options[] = {
    { "i-vars", 0, NULL, 'i'},
    { "m-vars", 0, NULL, 'm'},
    { NULL,     0, NULL, 0}
  };
  int c;
  int ivars, mvars;
  mvars=0;
  ivars=0;

  int i;				// standard loop counter

  while( 1) {
    c=getopt_long( argc, argv, "im", long_options, NULL);
    if( c == -1)
      break;

    switch( c) {
    case 'i':
      ivars=1;
      break;

    case 'm':
      mvars=1;
      break;

    }
  }

  stdinfda.fd = 0;
  stdinfda.events = POLLIN;

  initscr();				// Start ncurses
  raw();				// Line buffering disabled, control chars trapped
  keypad( stdscr, TRUE);		// Why is F1 nifty?
  refresh();

  pthread_mutex_init( &ncurses_mutex, NULL);	// don't lock this mutex yet because we are not multi-threaded until the "_run" functions

  //
  // Since the modules reference objects in other modules it is important
  // that everyone is initiallized before anyone runs
  //
  lspmac_init( ivars, mvars);
  lspg_init();
  md2cmds_init();

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

  lspmac_run();
  lspg_run();
  md2cmds_run();

  while( 1) {
    //
    // Big loop
    //

    nfd = 0;

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
	if( fda[i].fd == 0) {
	  stdinService( &fda[i]);
	}
      }
    }
  }
}

