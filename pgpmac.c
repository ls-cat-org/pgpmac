/*! \file pgpmac.c
 *  \brief Main for the pgpmac project.
 *  \date 2012
 *  \author Keith Brister
 *  \copyright All Rights Reserved
 *
 * \mainpage The LS-CAT pgpmac Project
 *
 *
 *  \details
 * pgpmac.c
 *
 * Some pmac defines, typedefs, functions suggested by Delta Tau
 * Accessory 54E User Manual, October 23, 2003 (C) 2003 by Delta Tau Data
 * Systems, Inc.  All rights reserved.
 *
 * 
 *
 * Original work Copyright (C) 2012 by Keith Brister, Northwestern University, All rights reserved.
 *
 * This project implements the MD2 communications required for
 * operation at LS-CAT and is intended to replace Windows XP based
 * .NET code provided by MAATEL.
 *
 * The need to do this is driven by a desire to make the system as
 * effecient and fast as possible by combining various operations.  A
 * proof-of-principle version of this code saw frame rates of
 * 23/minute as opposed to the nominal 18/minute we normally quote for
 * 1 second exposures.
 *
 * Additionally, as we rapidly approach EOL for Windows XP an
 * alternative is urgently needed.
 *
 * <h3>Structure</h3>
 *
 * The project is roughly broken down as follows:
 * <pre>
 *  lsevents.c		Simple event queue
 *
 *  lsredis.c		Receive key value pair updates from redis databases
 *
 *  lslogging.c		A logging utility to simplify debugging
 *
 *  lspg.c		Handles communications with the controlling posgresql database
 *
 *  lsupdate.c		Periodically update the px.kvs table with new positions.
 *
 *  md2cmds.c		Provides the equivilant (mostly) of the LS-CAT BLUMax code.
 *
 *  pgpmac.c		Main: parses command line and starts up the various threads
 *
 *  pgpmac.h		All includes and defines.  The only file included by the .c files in this project.
 *
 *  pmac_md2_ls-cat.pmc Code for the PMAC: compile and install with pmac exectutive program.
 *
 *  pmac_md2.sql        Tables and procedures for the posgresql side of the project.
 * </pre>
 *
 * <b>Notes:</b>
 *<ul>
 *	<li> The postgresql and the pmac communications interfaces are
 *	asynchronous and rely heavyly on the unix "poll" routine. </li>
 *
 *	<li> The project is multithreaded and based on
 *	"pthreads".</li>
 *	
 *	<li> Most threads maintain a queue of commands to simpify
 *	communications with each other.</li>
 *	
 *	<li> Note that a MAATEL supported interface for a more recent
 *	version of Windows may be available, however, a bit of effort
 *	will be required to implement it at LS-CAT as the BLUMax code
 *	will likely require some revisions.  This is still an option
 *	should the present project become intractable.</li>
 *
 *	<li> An important constraint has been to run the MD2 either
 *	from the windows .NET environment or from the pgpmac
 *	environment.  A consequence is that the pmac "pmc" file has
 *	been augmented to include new capabilities without destroying
 *	the code that the .NET interface requires.</li>
 *
 *      <li> Epics support could come by adapting the "e.c" code to
 *	work here directly or could come by making use of the existing
 *	kv pair mechanism already in place or, as is most likely, a
 *	combination of the two.</li>
 *
 *	<li> Ncurses support could include input lines for SQL queries
 *	and direct commands for supporting homing etc.  Perhaps the F
 *	keys could change modes or use of special mode changing text
 *	commands.  Output is not asynchronous.  Although this is
 *	unlikely to cause a problem I'd hate to have the program hang
 *	because terminal output is hung up.</li>
 *
 *	<li> PG queries come back as text instead of binary.  We could
 *	reduce the numeric errors by using binary and things would run
 *	a tad faster, though it is unlikely anyone would notice or
 *	care about the speed. </li>
 *
 *</ul>
 *
 * <h3>MD2 Motors and Coordinate Systems</h3>
 *
<pre>
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

</pre>

**
** MD2 Motion Programs
**

<pre>

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
</pre>
*/

#include "pgpmac.h"

WINDOW *term_output;			//!< place to print stuff out
WINDOW *term_input;			//!< place to put the cursor
WINDOW *term_status;			//!< shutter, lamp, air, etc status
WINDOW *term_status2;			//!< shutter, lamp, air, etc status

pthread_mutex_t ncurses_mutex;		//!< allow more than one thread access to the screen
int doomsday_count = 1;			//!< Countdown to quitting time: cleanout routines can request a few more heartbeats
pthread_mutex_t doomsday_mutex;		//!< avoid thread collision while resetting the doomsday clock

int pgpmac_use_pg = 0;			//!< non-zero to start up lspg thread, 0 to not (reids hash PG in config.HOSTNAME sets this)
int pgpmac_use_autoscint = 0;		//!< non-zero to automatically move the alignment stage when the scintillator moves (redis hash AUTOSCINT in config.HOSTNAME sets this)

//
// globals
//
static struct pollfd stdinfda;			//!< Handle input from the keyboard
static int running = 1;


/** Handle keyboard input
 */
#define PGPMAC_COMMAND_LINE_LENGTH 128
#define PGPMAC_N_COMMAND_LINES     128
void stdinService(
		  struct pollfd *evt		/**< [in] The pollfd object that caused this call	*/
		  ) {
  static char cmd_lines[PGPMAC_N_COMMAND_LINES][PGPMAC_COMMAND_LINE_LENGTH];
  static int current_line = 0;
  static int previous_line = 0;
  static char *cmdsp;
  static char *prompt = "PMAC>";;
  static int cmds_on = 0;
  static int cmd_mode = 0;
  static char cevt[32];
  int ch;
  int i;
  char tmp;


  cmdsp  = cmd_lines[current_line];

  for( ch=wgetch(term_input); ch != ERR && running; ch=wgetch(term_input)) {

    switch( ch) {
    case KEY_F(1):
    case KEY_F(2):
    case KEY_F(3):
      lspmac_abort();					// send abort now (as opposed to an event listener) in case a cleanup routine wants to move something (we don't want to abort it).
      lsevents_send_event( "Quitting Program");		// let everyone know the end is nigh
      lstimer_set_timer( "Quit Program", -1, 1, 0);	// Doomsday, repeat as needed
      break;

    case 0x0002:	// Control-B    Report status word for 8 motors
    case 0x0003:	// Control-C    Report all coordinate system status words
    case 0x0006:	// Control-F    Report following errors for 8 motors
    case 0x0010:	// Control-P    Report positions for 8 motors
    case 0x0016:	// Control-V    Report velocity on 8 motors
      sprintf( cevt, "Control-%c", '@' + ch);
      lspmac_SockSendControlCharPrint( cevt, ch);
      break;

    case 0x0001:	// Control-A	Abort all programs and moves
    case 0x0004:	// Control-D    Disable all PLC programs
    case 0x0005:	// Control-E    Enable disabled motors
    case 0x0007:	// Control-G    Report global status word
    case 0x000b:	// Control-K    Kill all motors
    case 0x000f:	// Control-O    Feed hold on all coordinate systems
    case 0x0011:	// Control-Q	Quit all executing motion programs
    case 0x0012:	// Control-R	Run motion programs in all coordinate systems
    case 0x0013:	// Control-S    Step through working motion programs in all coordinate systems
    case 0x0018:	// Control-X    Cancel in-process communications
      sprintf( cevt, "Control-%c", '@' + ch);
      lspmac_SockSendControlCharPrint( cevt, ch);
      //      lspmac_SockSendDPControlChar( cevt, ch);
      break;

    case 0x000c:	// Control-L
      pthread_mutex_lock( &ncurses_mutex);
      redrawwin( term_status);
      redrawwin( term_status2);
      redrawwin( term_output);
      redrawwin( term_input);
      for( i=0; i<lspmac_nmotors; i++) {
	if( lspmac_motors[i].win != NULL)
	  redrawwin( lspmac_motors[i].win);
      }
      pthread_mutex_unlock( &ncurses_mutex);
      break;

    case KEY_UP:
      previous_line = (previous_line - 1 + PGPMAC_N_COMMAND_LINES) % PGPMAC_N_COMMAND_LINES;
      if( previous_line == current_line || cmd_lines[previous_line][0] == 0) {
	//
	// We seem to have gone through all the lines, but NO MORE.
	//
	previous_line = (previous_line + 1) % PGPMAC_N_COMMAND_LINES;
      }

      memset( cmdsp, 0, PGPMAC_COMMAND_LINE_LENGTH);
      strcpy( cmdsp, cmd_lines[previous_line]);
      cmds_on = strlen(cmdsp);
      break;

    case KEY_DOWN:
      if( previous_line != current_line)
	previous_line = (previous_line + 1) % PGPMAC_N_COMMAND_LINES;

      memset( cmdsp, 0, PGPMAC_COMMAND_LINE_LENGTH);
      strcpy( cmdsp, cmd_lines[previous_line]);
      cmds_on = strlen(cmdsp);
      break;

    case KEY_LEFT:
      cmds_on = cmds_on == 0 ? 0 : cmds_on - 1;
      break;

    case KEY_RIGHT:
      cmds_on = cmds_on >= strlen(cmdsp) ? strlen(cmdsp) : cmds_on + 1;
      break;

    case KEY_BACKSPACE:
      cmds_on == 0 ? 0 : cmds_on--;
      for( i=0; *(cmdsp + cmds_on + i) != 0; i++) {
	*(cmdsp + cmds_on + i) = *(cmdsp + cmds_on + i + 1);
      }
      break;
      
    case KEY_ENTER:
    case 0x000a:
      if( cmds_on > 0 && strlen( cmdsp) > 0) {
	switch( cmd_mode) {
	case 0:
	  if( strcmp( cmdsp, "$$$") == 0) {
	    lsevents_send_event( "Full Card Reset Requested");
	    lslogging_log_message( "Performing Full Card Reset, resuming in 10 seconds");
	    lstimer_set_timer( "Full Card Reset", 1, 10, 0);
	  }
	  lspmac_SockSendline( NULL, "%s", cmdsp);
	  break;
	case 1:
	  md2cmds_push_queue( cmdsp);
	  break;
	}
      }
      current_line = (current_line + 1) % PGPMAC_N_COMMAND_LINES;
      previous_line = current_line;
      cmdsp = cmd_lines[current_line];
      memset( cmdsp, 0, PGPMAC_COMMAND_LINE_LENGTH);
      cmds_on = 0;
      break;
      
    default:
      if( ch >= 0x20 && ch <= 0x7e) {
	if( cmds_on < PGPMAC_COMMAND_LINE_LENGTH - 1) {
	  for( i=cmds_on; ch != 0 && i < PGPMAC_COMMAND_LINE_LENGTH; i++) {
	    tmp = *(cmdsp + i);
	    *(cmdsp + i) = ch;
	    ch = tmp;
	  }
	  cmds_on = (cmds_on + 1) % PGPMAC_COMMAND_LINE_LENGTH;
	}
      }
      break;
    }
    
    if(strcasecmp( "pmac", cmdsp) == 0) {
      *cmdsp   = 0;
      cmd_mode = 0;
      cmds_on  = 0;
      memset( cmdsp, 0, PGPMAC_COMMAND_LINE_LENGTH);
      prompt = "PMAC>";
    }

    if(strcasecmp( "md2cmds", cmdsp) == 0) {
      *cmdsp   = 0;
      cmd_mode = 1;
      cmds_on  = 0;
      memset( cmdsp, 0, PGPMAC_COMMAND_LINE_LENGTH);
      prompt = "md2cmds>";
    }

    if( strcasecmp( "quit", cmdsp) == 0) {
      lspmac_abort();					// send abort now (as opposed to an event listener) in case a cleanup routine wants to move something (we don't want to abort it).
      lsevents_send_event( "Quitting Program");		// let everyone know the end is nigh
      lstimer_set_timer( "Quit Program", -1, 1, 0);	// Doomsday, repeat as needed
      *cmdsp   = 0;
      cmds_on  = 0;
      memset( cmdsp, 0, PGPMAC_COMMAND_LINE_LENGTH);
    }

    if( running) {
      pthread_mutex_lock( &ncurses_mutex);
      mvwprintw( term_input, 1, 1, "%s %s", prompt, cmdsp);
      wclrtoeol( term_input);
      wmove( term_input, 1, cmds_on + strlen(prompt) + 2);
      box( term_input, 0, 0);
      wnoutrefresh( term_output);
      wnoutrefresh( term_input);
      doupdate();
      pthread_mutex_unlock( &ncurses_mutex);
    }
  }
}

/** Terminal output routine ala printf
 */
void pgpmac_printf(
		   char *fmt,		/**< [in]  Printf style formating string		*/
		   ...			/*         Arguments required by fmt			*/
		   ) {
  va_list arg_ptr;

  pthread_mutex_lock( &ncurses_mutex);

  va_start( arg_ptr, fmt);
  vwprintw( term_output, fmt, arg_ptr);
  va_end( arg_ptr);

  wnoutrefresh( term_output);
  doupdate();
  pthread_mutex_unlock( &ncurses_mutex);

}

/** Postpone the day of reckoning
 *  This assumes the quit_cb routine is called once a second.
 */
void pgpmac_request_stay_of_execution( int secs) {
  pthread_mutex_lock( &doomsday_mutex);
  if( secs > doomsday_count)
    doomsday_count = secs;
  pthread_mutex_unlock( &doomsday_mutex);
}


/** quit the program
 */
void pgpmac_quit_cb( char *event) {
  pthread_mutex_lock( &doomsday_mutex);
  doomsday_count--;
  if( doomsday_count <= 0)
    running = 0;
  pthread_mutex_unlock( &doomsday_mutex);
}


/** Our main routine
 */
int main(
	 int argc,		/**< [in] Number of arguments			*/
	 char **argv		/**< [in] Vector of argument strings		*/
	 ) {

  static struct pollfd fda[3];		// input for poll: room for postgres, pmac, and stdin
  static int nfd = 0;			// number of items in fda
  static int pollrtn = 0;
  static struct option long_options[] = {
    { "i-vars", 0, NULL, 'i'},
    { "m-vars", 0, NULL, 'm'},
    { NULL,     0, NULL, 0}
  };
  int c;
  int ivars, mvars;		//!< argument flags
  mvars    = 0;
  ivars    = 0;
  int i;			// standard loop counter
  pthread_mutexattr_t mutex_initializer;
  static sigset_t our_sigset;

  //
  // We use SIGUSR1 but sometimes it's called before a handler is in place so we'll just block it
  // here and now.
  //
  sigemptyset( &our_sigset);
  sigaddset( &our_sigset, SIGUSR1);
  pthread_sigmask(SIG_BLOCK, &our_sigset, NULL);

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

  // Use recursive mutexs
  //
  pthread_mutexattr_init( &mutex_initializer);
  pthread_mutexattr_settype( &mutex_initializer, PTHREAD_MUTEX_RECURSIVE);

  pthread_mutex_init( &ncurses_mutex, &mutex_initializer);	// don't lock this mutex yet because we are not multi-threaded until the "_run" functions
  pthread_mutex_init( &doomsday_mutex, &mutex_initializer);

  //
  // Since the modules reference objects in other modules it is important
  // that everyone is initiallized before anyone runs
  //
  // Everyone needs to be able to log messages
  lslogging_init();
  lslogging_run();

  // Everyone needs to send and listen for events
  //
  lsevents_init();
  lsevents_run();

  //
  // Add a couple of our own
  //
  lsevents_add_listener( "^Quit Program$", pgpmac_quit_cb);
  lsevents_preregister_event( "Quit Program");
  lsevents_preregister_event( "Quitting Program");

  //
  // Timers are needed by all too
  //
  lstimer_init();
  lstimer_run();

  //
  // Redis is where we get our configuration
  // as well as one of communicating with the outside world
  //
  lsredis_init();
  lsredis_run();
  lsredis_config();

  //
  // These need to be all initialized before any are run
  //
  lspmac_init( ivars, mvars);

  if( pgpmac_use_pg)
    lspg_init();

  md2cmds_init();

  //
  // set up our screen
  //
  pthread_mutex_lock( &ncurses_mutex);
  term_status = newwin( LS_DISPLAY_WINDOW_HEIGHT, LS_DISPLAY_WINDOW_WIDTH, 3*LS_DISPLAY_WINDOW_HEIGHT, 0*LS_DISPLAY_WINDOW_WIDTH);
  box( term_status, 0, 0);
  wnoutrefresh( term_status);
						      
  term_status2 = newwin( LS_DISPLAY_WINDOW_HEIGHT, LS_DISPLAY_WINDOW_WIDTH, 3*LS_DISPLAY_WINDOW_HEIGHT, 1*LS_DISPLAY_WINDOW_WIDTH);
  box( term_status2, 0, 0);
  wnoutrefresh( term_status2);
						      
  term_output = newwin( 20, 5*LS_DISPLAY_WINDOW_WIDTH, 4*LS_DISPLAY_WINDOW_HEIGHT, 0);
  scrollok( term_output, 1);			      
  wnoutrefresh( term_output);			      
						      
  term_input  = newwin( 3, 5*LS_DISPLAY_WINDOW_WIDTH, 20+4*LS_DISPLAY_WINDOW_HEIGHT, 0);
  box( term_input, 0, 0);			      
  mvwprintw( term_input, 1, 1, "PMAC> ");	      
  nodelay( term_input, TRUE);			      
  keypad( term_input, TRUE);			      
  wnoutrefresh( term_input);			      
						      
  doupdate();					      
  pthread_mutex_unlock( &ncurses_mutex);

  //
  // Now run the world
  //
  lspmac_run();

  if( pgpmac_use_pg)
    lspg_run();

  md2cmds_run();

  while( running) {
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
  endwin();
  return 0;
}

