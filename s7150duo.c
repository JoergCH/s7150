/* vi:set syntax=c expandtab tabstop=4 shiftwidth=4:

 S 7 1 5 0 D U O . C

 Controls two Solartron 7150 (and 7150-plus) Digital Multimeters using GPIB.

 Copyright (c) 2004...2025 by Joerg Hau.

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License version 2 as
 published by the Free Software Foundation, provided that the copyright
 notice remains intact even in future versions. See the file LICENSE
 for details.

 If you use this program (or any part of it) in another application,
 note that the resulting application becomes also GPL. In other
 words, GPL is a "contaminating" license.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 --------------------------------------------------------------------

 Modification/history (adapt VERSION below when changing!):

 2004-12-19     creation based on s7150.c (JHa)
 2005-01-02     improved gnuplot stuff (JHa)
 2005-12-11     added comment; leave plot window open 
                after acquisition until keypress (JHa)
 2009-01-28     added timeout (JHa). Added patch for S7150plus and
                context dependent ylabel (JJL)
 2016-02-17     bugfix as in s7150.c, updated doc (JHa)
 2017-01-06     updated doc (JHa)
 2025-08-11    moved everything to GitHub (JHa)
 
 This should compile with any C compiler, something like:

 gcc -Wall -O2 -lgpib -o s7150duo s7150duo.c

 Make sure the user accessing GPIB is in group 'gpib'.

*/

#define VERSION "V20250811"     /* String! */

//#define DEBUG           /* diagnostic mode, for development only */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <errno.h>          /* command line reading */
#include <unistd.h>
#include <termios.h>        /* kbhit() */
#include <sys/io.h>
#include <sys/time.h>       /* clock timing */
#include "gpib/ib.h"

#define MAXLEN   90         /* text buffers etc */
#define ESC      27
#define GNUPLOT  "gnuplot"   /* gnuplot executable */

#define ERR_FILE  4         /* error code */
#define ERR_INST  5         /* error code */

#define GPIB_BOARD_ID 0     /* GPIB card #, default is 0 */



/* --- stuff for reading the command line --- */

char *optarg;               /* global: pointer to argument of current option */
int optind = 1;             /* global: index of which argument is next. Is used
                            as a global variable for collection of further
                            arguments (= not options) via argv pointers */

/* --- stuff for kbhit() ---- */

static  struct termios initial_settings, new_settings;
static  int peek_character = -1;

void    init_keyboard(void);
void    close_keyboard(void);
int     kbhit(void);
int     readch(void);

/* --- miscellaneous function prototypes ---- */

double  timeinfo (void);
int     strclean (char *buf);
int     GetOpt (int argc, char *argv[], char *optionS);

/* --- s7150-related function prototypes ---- */

int     s7150_open (const int adr);
int     s7150_setup (const int dvm, const int display, \
                     const int fun, const int range, const float freq);
int     s7150_read (const int dvm, const int delay, char *result);
int     s7150_close (const int adr);

/* --- things to make life easier ---- */
#ifdef PLUS
enum s7150_function  { DCV = 0, ACV, OHM, DCA, ACA, DIODE, DEGC, DEGF };
#else
enum s7150_function  { DCV = 0, ACV, OHM, DCA, ACA, DIODE };
#endif
enum s7150_range_v   { VAUTO = 0, V02, V2, V20, V200, V2000 };
enum s7150_range_ma  { AAUTO = 0, A2000 = 5 };
enum s7150_range_ohm { RAUTO = 0, R20K = 3, R200K, R2M, R20M };


/********************************************************
* main:       main program loop.                        *
* Input:      see below.                                *
* Return:     0 if OK, else error code                  *
********************************************************/
int main (int argc, char *argv[])
{
static char *disclaimer =
"\ns7150duo - Data acquisition using two Solartron 7150 over GPIB. " VERSION ".\n"
"Copyright (C) 2004...2025 by Joerg Hau.\n\n"
"This program is free software; you can redistribute it and/or modify it under\n"
"the terms of the GNU General Public License, version 2, as published by the\n"
"Free Software Foundation.\n\n"
"This program is distributed in the hope that it will be useful, but WITHOUT ANY\n"
"WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A\n"
"PARTICULAR PURPOSE. See the GNU General Public License for details.\n";

static char *msg = "\nSyntax: s7150duo [-h] [-n] [-w samp] [-[a|A] id] [-[m|M] mode] [-d] [-t dt] [-T timeout] [-c \"txt\"] [-g /path/to/gnuplot] [-f] datafile"
"\n        -h       this help screen"
"\n        -a id    use instrument 1 at GPIB address 'id' (default is 16)"
"\n        -A id    use instrument 2 at GPIB address 'id' (default is 12)"
"\n        -m mod   measurement mode instrument 1 (default is DCV)"
"\n        -M mod   measurement mode instrument 2 (default is DCA)"
"\n        -t dt    delay between measurements in 0.1 s (default is 10)"
"\n        -d       disable instrument display (default is on)"
"\n        -w x     force write to disk every x samples (default is 100)"
"\n        -f       force overwriting of existing file"
"\n        -T min   stop acquisition after this time (in minutes; default 0 = endless)"
"\n        -c txt   comment text"
"\n        -g       specify path/to/gnuplot (if not in your current PATH)"
"\n        -n       no graphics\n\n";

#ifdef PLUS
static char *ylabels[] = {"V","V","kOhms","mA","mA","mV","deg C","deg F"};
#else
static char *ylabels[] = {"V","V","kOhms","mA","mA","mV"};
#endif

FILE    *outfile, *gp = NULL;
char    buffer1[MAXLEN], buffer2[MAXLEN], filename[MAXLEN], comment[MAXLEN] = "", gnuplot[MAXLEN];
char    do_display = 1, do_graph = 1, do_overwrite = 0;
int     dvm1, dvm2, pad1 = 16, pad2 = 12, key, do_flush = 100, delay = 10, \
	    mode1 = DCV, mode2 = DCA, range = 0;
unsigned long loop = 0L;
double  t0, t1;
float   tstop = 0.0;
time_t  t;

/* --- set the executable --- */

sprintf (gnuplot, "%s", GNUPLOT);

/* --- show the usual text --- */

fprintf (stderr, disclaimer);

/* --- decode and read the command line --- */

while ((key = GetOpt(argc, argv, "hfnda:A:w:t:T:m:c:M:g:")) != EOF)
    switch (key)
        {
        case 'h':                    /* help me */
            fprintf (stderr, msg);
            return 0;
        case 'f':                    /* force overwriting of existing file */
            do_overwrite = 1;
            continue;
        case 'n':                    /* disable graph display */
            do_graph = 0;
            continue;
        case 'd':                    /* disable display */
            do_display = 0;
            continue;
        case 'c':
            if (strclean (optarg))    
                strcpy (comment, optarg);
            continue;
        case 'g':
            sscanf (optarg, "%80s", gnuplot);
            continue;
        case 'w':
            sscanf (optarg, "%5d", &do_flush);
            continue;
        case 'a':
            sscanf (optarg, "%5d", &pad1);
            if (pad1 < 0 || pad1 > 30)
                {
                puts("Error: primary address must be between 0 and 30.");
                return 1;
                }
	    continue;
        case 'A':
            sscanf (optarg, "%5d", &pad2);
            if (pad2 < 0 || pad2 > 30)
                {
                puts("Error: primary address must be between 0 and 30.");
                return 1;
                }
            continue;
	/* FIXME: Check that two different adresses are given */
        case 't':
            sscanf (optarg, "%5d", &delay);
            if (delay < 0 || delay > 600)
                {
                puts("Error: delay must be 0 ... 600 (1/10 s).");
                return 1;
                }
            continue;
        case 'T':
            sscanf (optarg, "%g", &tstop);
            if (tstop < 0.0)
                {
                puts("Error: timeout must be positive.");
                return 1;
                }
            continue;
        case 'm':
            sscanf (optarg, "%5d", &mode1);
#ifdef PLUS
            if (mode1 < 0 || mode1 > 7)
                {
                puts("Error: mode1 must be 0 ... 7.");
                puts("0 = DCV, 1 = ACV, 2 = Ohm, 3 = DCA, 4 = ACA, 5 = Diode, 6 = DEGC, 7 = DEGF");
                return 1;
                }
#else
            if (mode1 < 0 || mode1 > 5)
                {
                puts("Error: mode1 must be 0 ... 5.");
                puts("0 = DCV, 1 = ACV, 2 = Ohm, 3 = DCA, 4 = ACA, 5 = Diode");
                return 1;
                }
#endif
            continue;
        case 'M':
            sscanf (optarg, "%5d", &mode2);
#ifdef PLUS
            if (mode2 < 0 || mode2 > 7)
                {
                puts("Error: mode2 must be 0 ... 7.");
                puts("0 = DCV, 1 = ACV, 2 = Ohm, 3 = DCA, 4 = ACA, 5 = Diode, 6 = DEGC, 7 = DEGF");
                return 1;
                }
#else
            if (mode2 < 0 || mode2 > 5)
                {
                puts("Error: mode2 must be 0 ... 5.");
                puts("0 = DCV, 1 = ACV, 2 = Ohm, 3 = DCA, 4 = ACA, 5 = Diode");
                return 1;
                }
#endif
            continue;
        case 'r':
            /* no range check yet, this would require a lot of
               cross-checking against the range capabilities */
        case '~':                    /* invalid arg */
        default:
        fprintf (stderr, "'%s -h' for help.\n\n", argv[0]);
            return 1;
        }

if (argv[optind] == NULL)       /* we need at least one parameter on cmd line */
    {
    fprintf (stderr, msg);
    fprintf (stderr, "Please specify a data file.\n");
    return 1;
    }

/* --- prepare output data file --- */

strcpy (filename, argv[optind]);
if ((!access(filename, 0)) && (!do_overwrite))
/* If file exists and overwrite is NOT forced */
    {
    fprintf (stderr, "\a\nFile '%s' exists - Overwrite? [Y/*] ", filename);
    key = fgetc(stdin);         // read from keyboard
    switch (key)
        {
        case 'Y':
        case 'y':
            break;
            default:
            return 1;
        }
    }

if (NULL == (outfile = fopen(filename, "wt")))
    {
    fprintf(stderr, "Could not open '%s' for writing.\n", filename);
    pclose(gp);
    return ERR_FILE;
    }

/* --- real-time display: prepare gnuplot for action --- */

gp = popen(gnuplot,"w");
if (NULL == gp)
    {
    fprintf(stderr, "\nCannot launch gnuplot, will continue \"as is\".\n") ;
    fflush(stderr);
    do_graph = 0;    /* do NOT abort here, just continue */
    }

if (do_graph)       /* set gnuplot display defaults */
    {
    fprintf(gp, "set mouse;set mouse labels; set style data lines; set title '%s'\n", filename);
    fprintf(gp, "set grid xt; set grid yt; set xlabel 'min'; set ylabel '%s'\n", ylabels[mode1]);
    fprintf(gp, "set y2label '%s'; set y2tics\n", ylabels[mode2]);
    fflush (gp);
    }

init_keyboard();    /* for kbhit() functionality */

/* preparations are finished, now let's get it going ... */

dvm1 = s7150_open(pad1);
dvm2 = s7150_open(pad2);
if ((dvm1 == 0) || (dvm2 == 0))
    {
    fprintf(stderr, "Quit.\n");
    pclose(gp);
    return ERR_INST;
    }

/* we control two instruments, so let's divide the delay. 
   FIXME: To be "really" correct, the delay should be applied 
   once and then both instruments read immediately one after the other */  
delay = delay/2.0;

if ((0 == s7150_setup(dvm1, do_display, mode1, range, 10.0/delay)) || \
    (0 == s7150_setup(dvm2, do_display, mode2, range, 10.0/delay)))
    {
    fprintf(stderr, "Quit.\n");
    pclose(gp);
    return ERR_INST;
    }

printf("\n GPIB address :  %d and %d", pad1, pad2);
printf("\n  Output file :  %s", filename);
if (strlen(comment))
	printf("\n      Comment :  %s", comment);
printf("\n     Sampling :  %.1f s", 2.0*delay/10.0);
printf("\n      Refresh :  %d", do_flush);
if (tstop > 0.0)
    printf("\n   Halt after :  %g min", tstop);
printf("\n         Stop :  Press 'q' or ESC.\n");
printf("\n     Count           Time      Reading\n");
fflush(stdout);

/* Get time, write file header */
time(&t);
fprintf(outfile, "# s7150duo " VERSION "\n");
fprintf(outfile, "# %s\n", comment);
fprintf(outfile, "# Acquisition start: %s", ctime(&t));
fprintf(outfile, "# min\treadout  errflag  unit  mode  unit mode\n");
t0 = timeinfo();

key = 0;
do  {
    /* delay = 0 means free-running acquisition with highest speed */
    if ((0 == (s7150_read(dvm1, delay, buffer1))) ||
        (0 == (s7150_read(dvm2, delay, buffer2))))
        {
        fprintf(stderr, "Quit.\n");
        pclose(gp);
        close_keyboard();
        return ERR_INST;
        }

    t1 = (timeinfo()-t0)/60.0;
    printf("%10lu %10.2f min    %s\t%s\r", ++loop, t1, buffer1, buffer2);
    fprintf(outfile, "%.4f\t%s\t%s\n", t1, buffer1, buffer2); // write literally to file
    fflush (stdout);
    
    /* handle timeout */
    if ((t1 > tstop) && (tstop > 0.0))
        key = ESC;

    /* ensure write & display at least every x data points */
    if (!(loop % do_flush))
        {
        fflush (outfile);
        if (do_graph)
            {
            fprintf(gp, "plot '%s' using 1:2 title '%d: %s', '' using 1:5 title '%d: %s'\n", \
                    filename, pad1, ylabels[mode1], pad2, ylabels[mode2]);
            fflush (gp);
            }
        }

    /* look up keyboard for keypress */
    if(kbhit())
        key = readch();
    }
    while ((key != 'q') && (key != ESC));

time(&t);
fprintf(outfile, "# Acquisition stop: %s\n", ctime(&t));
fclose (outfile);

/* send reset to instrument */
if ((! s7150_close(dvm1)) ||
    (! s7150_close(dvm2)))
    {
    fprintf(stderr, "Quit.\n");
    return ERR_INST;
    }

if (do_graph)   /* if graphic display was used, replot data and wait for keypress */
    {
   fprintf(gp, "plot '%s' using 1:2 title '%d: %s', '' using 1:5 title '%d: %s'\n", \
           filename, pad1, ylabels[mode1], pad2, ylabels[mode2]);	fflush (gp);
	printf("\nAcquisition finished. Press any key to terminate graphic display and exit.\n");
    while (!kbhit())
	    usleep (100000); 	/* wait 0.1 s */
	pclose(gp);
   }

close_keyboard();   /* close kbhit() stuff properly */
printf("\n\n");
return 0;
}


/********************************************************
* s7150_open: Connect and initialise the Solartron 7150 *
* Input:      GPIB address                              *
* Return:     0 if error, file descriptior if OK        *
********************************************************/
int s7150_open (const int pad)
{
int dvm;
static char buf[MAXLEN];

dvm = ibdev(GPIB_BOARD_ID, pad, 0, T1s, 1, 0);
if(dvm < 0)
    {
    fprintf(stderr, "Error trying to open GPIB address %i\n", pad);
    return 0;
    }

/*  A = device clear (I would prefer DC1 but this gives an error msg)
    U7 = CR as delimiter (U0 = CR,LF),
    N0 = verbose output
    T1 = tracking on (T0 = single-shot)
    I3 = integration 400 ms
    */

strcpy (buf, "A\n");
if (ibwrt(dvm, buf, strlen(buf)) & ERR )
    {
    fprintf(stderr, "Error during init step 1 of GPIB address %i!\n", pad);
    return 0;
    }
sleep (2);
strcpy (buf, "U7N0T1\n");
if (ibwrt(dvm, buf, strlen(buf)) & ERR )
    {
    fprintf(stderr, "Error during init step 2 of GPIB address %i!\n", pad);
    return 0;
    }

/* arrive here if OK */
return dvm;
}


/********************************************************
* s7150_setup: Sets operating mode of the S7150.        *
* Input:    - file pointer as delivered by s7150_open() *
*           - display, function, range, acq freq in Hz  *
* Return:   1 if OK, 0 if error                         *
********************************************************/
int s7150_setup (const int dvm, const int display, const int fun, \
                 const int range, const float freq)
{
int d = 0, i = 3;
static char buf[MAXLEN];

/* note: the 7150 uses "D1" to switch the display OFF */
if (display == 0)
    d = 1;

/*  integration rate: I0 = 6.7 ms
                      I1 = 40 ms (for 50 Hz line freq)
                      I3 = 400 ms
                      I4 = average
    Here, integration time is set to I1, if sampling intervals
    of less than 1 s are desired. I0 would be used in case of
    a free-running acquisition. */

if (freq < 0.25)   /* less than 1 sample in 4 s */
    i = 4;
if (freq > 1.5)    /* more than 1.5 Hz */
    i = 1;
if (freq > 10.0)   /* more than 10 Hz */
    i = 0;

#ifdef DEBUG
    fprintf(stderr, "%.2f Hz -> using I%d.\n", freq, i);
#endif

sprintf (buf, "D%dM%dR%dI%d\n", d, fun, range, i);
if (ibwrt(dvm, buf, strlen(buf)) & ERR )
    {
    fprintf(stderr, "Error during mode setting!\n");
    return 0;
    }
return 1;
}


/********************************************************
* s7150_read: Reads value from the Solartron 7150.      *
* Input:    - file ptr as delivered by s7150_open()     *
*           - delay between measurements (in 0.1 s)     *
*           - ptr to char for result                    *
* Return:   1 if OK, 0 if error                         *
********************************************************/
int s7150_read (const int dvm, const int delay, char *result)
{
static char buf[2];

if (delay > 0)	/* delay == 0 is free-running */
    {
    strcpy (buf, "G");
/*    if (ibwrt(dvm, buf, strlen(buf)) & ERR )
        {
        fprintf(stderr, "Error trying to initiate measurement!\n");
        return 0;
        }
*/    usleep (delay * 100000);
    }

/* Solartron 7150 puts out 15 chars plus LF */
if(ibrd(dvm, result, 16) & ERR)
    {
    fprintf(stderr, "Error trying to read from instrument!\n");
    return 0;
    }

/* make sure string is null-terminated;
   at the same time, cut off CR  */
result[ibcnt-1] = 0x0;  

//printf("\nreceived string:'%s', number of bytes read: %i\n", result, ibcnt);
// FIXME: errcheck: char 11 != '!'

return 1;
}



/********************************************************
* s7150_close: Reset and disconnect Solartron 7150      *
* Input:    file pointer as delivered by s7150_open()   *
* Return:   0 if error, 1 if OK                         *
********************************************************/
int s7150_close (const int dvm)
{
static char buf[10];

strcpy (buf, "DC1\nA\n");
if (ibwrt(dvm, buf, strlen(buf)) & ERR )
    {
    fprintf(stderr, "Error during reset of instrument!\n");
    return 0;
    }
return 1;
}


/********************************************************
* TIMEINFO: Returns actual time elapsed since The Epoch *
* Input:    Nothing.                                    *
* Return:   time in microseconds                        *
* Note:     #include <time.h>                           *
*           #include <sys/time.h>                       *
********************************************************/
double timeinfo (void)
{
struct timeval t;

gettimeofday(&t, NULL);
return (double)t.tv_sec + (double)t.tv_usec/1000000.0;
}


/************************************************************************
* Function:     strclean                                                *
* Description:  "cleans" a text buffer obtained by fgets()              *
* Arguments:    Pointer to text buffer                                  *
* Returns:      strlen of buffer                                        *
*************************************************************************/
int strclean (char *buf)
{
int i;

for (i = 0; i < strlen (buf); i++)    /* search for CR/LF */
    {
    if (buf[i] == '\n' || buf[i] == '\r')
        {
        buf[i] = 0;        /* stop at CR or LF */
        break;
        }
    }
return (strlen (buf));
}


/********************************************************
* KBHIT: provides the functionality of DOS's kbhit()    *
* found at http://linux-sxs.org/programming/kbhit.html  *
* Input:    Nothing.                                    *
* Return:   time in microseconds                        *
* Note:     #include <termios.h>                        *
********************************************************/
void init_keyboard (void)
{
tcgetattr( 0, &initial_settings );
new_settings = initial_settings;
new_settings.c_lflag &= ~ICANON;
new_settings.c_lflag &= ~ECHO;
new_settings.c_lflag &= ~ISIG;
new_settings.c_cc[VMIN] = 1;
new_settings.c_cc[VTIME] = 0;
tcsetattr( 0, TCSANOW, &new_settings );
}

void close_keyboard(void)
{
tcsetattr( 0, TCSANOW, &initial_settings );
}

int kbhit (void)
{
char ch;
int nread;

if( peek_character != -1 )
    return( 1 );
new_settings.c_cc[VMIN] = 0;
tcsetattr( 0, TCSANOW, &new_settings );
nread = read( 0, &ch, 1 );
new_settings.c_cc[VMIN] = 1;
tcsetattr( 0, TCSANOW, &new_settings );
if( nread == 1 )
    {
    peek_character = ch;
    return (1);
    }
return (0);
}

int readch (void)
{
char ch;

if( peek_character != -1 )
    {
    ch = peek_character;
    peek_character = -1;
    return( ch );
    }
/* else */
read( 0, &ch, 1 );
return( ch );
}


/***************************************************************************
* GETOPT: Command line parser, system V style.
*
*  Widely (and wildly) adapted from code published by Borland Intl. Inc.
*
*  Note that libc has a function getopt(), however this is not guaranteed
*  to be available for other compilers. Therefore we provide *this* function
*  (which does the same).
*
*  Standard option syntax is:
*
*    option ::= SW [optLetter]* [argLetter space* argument]
*
*  where
*    - SW is '-'
*    - there is no space before any optLetter or argLetter.
*    - opt/arg letters are alphabetic, not punctuation characters.
*    - optLetters, if present, must be matched in optionS.
*    - argLetters, if present, are found in optionS followed by ':'.
*    - argument is any white-space delimited string.  Note that it
*      can include the SW character.
*    - upper and lower case letters are distinct.
*
*  There may be multiple option clusters on a command line, each
*  beginning with a SW, but all must appear before any non-option
*  arguments (arguments not introduced by SW).  Opt/arg letters may
*  be repeated: it is up to the caller to decide if that is an error.
*
*  The character SW appearing alone as the last argument is an error.
*  The lead-in sequence SWSW ("--") causes itself and all the rest
*  of the line to be ignored (allowing non-options which begin
*  with the switch char).
*
*  The string *optionS allows valid opt/arg letters to be recognized.
*  argLetters are followed with ':'.  Getopt () returns the value of
*  the option character found, or EOF if no more options are in the
*  command line. If option is an argLetter then the global optarg is
*  set to point to the argument string (having skipped any white-space).
*
*  The global optind is initially 1 and is always left as the index
*  of the next argument of argv[] which getopt has not taken.  Note
*  that if "--" or "//" are used then optind is stepped to the next
*  argument before getopt() returns EOF.
*
*  If an error occurs, that is an SW char precedes an unknown letter,
*  then getopt() will return a '~' character and normally prints an
*  error message via perror().  If the global variable opterr is set
*  to false (zero) before calling getopt() then the error message is
*  not printed.
*
*  For example, if
*
*    *optionS == "A:F:PuU:wXZ:"
*
*  then 'P', 'u', 'w', and 'X' are option letters and 'A', 'F',
*  'U', 'Z' are followed by arguments. A valid command line may be:
*
*    aCommand  -uPFPi -X -A L someFile
*
*  where:
*    - 'u' and 'P' will be returned as isolated option letters.
*    - 'F' will return with "Pi" as its argument string.
*    - 'X' is an isolated option.
*    - 'A' will return with "L" as its argument.
*    - "someFile" is not an option, and terminates getOpt.  The
*      caller may collect remaining arguments using argv pointers.
***************************************************************************/
int GetOpt (int argc, char *argv[], char *optionS)
{
   static char *letP = NULL;    /* remember next option char's location */
   static char SW = '-';    /* switch character */

   int opterr = 1;      /* allow error message        */
   unsigned char ch;
   char *optP;

   if (argc > optind)
   {
      if (letP == NULL)
      {
     if ((letP = argv[optind]) == NULL || *(letP++) != SW)
        goto gopEOF;

     if (*letP == SW)
     {
        optind++;
        goto gopEOF;
     }
      }
      if (0 == (ch = *(letP++)))
      {
     optind++;
     goto gopEOF;
      }
      if (':' == ch || (optP = strchr (optionS, ch)) == NULL)
     goto gopError;
      if (':' == *(++optP))
      {
     optind++;
     if (0 == *letP)
     {
        if (argc <= optind)
           goto gopError;
        letP = argv[optind++];
     }
     optarg = letP;
     letP = NULL;
      }
      else
      {
     if (0 == *letP)
     {
        optind++;
        letP = NULL;
     }
     optarg = NULL;
      }
      return ch;
   }

 gopEOF:
   optarg = letP = NULL;
   return EOF;

 gopError:
   optarg = NULL;
   errno = EINVAL;
   if (opterr)
      perror ("\nCommand line option");
   return ('~');
}

