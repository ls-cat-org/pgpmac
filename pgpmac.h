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

#define LS_DISPLAY_WINDOW_HEIGHT 8
#define LS_DISPLAY_WINDOW_WIDTH  24
#define LS_PG_QUERY_STRING_LENGTH 1024


typedef struct lsDisplayStruct {
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
} ls_display_t;

extern ls_display_t ls_displays[];
extern int ls_ndisplays;

extern WINDOW *term_output;
extern WINDOW *term_input;
extern WINDOW *term_status;

void PmacSockSendline( char *s);
