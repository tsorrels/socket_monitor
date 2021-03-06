/* */
//#include <sys/types.h>
//#include <curses.h>
#include "netmonitor.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>

static pthread_mutex_t netstatmtx = PTHREAD_MUTEX_INITIALIZER;
struct State state;
void quit(int status);
void initialize();
void writeheader();
void writelines();
void runnetstat();
void boldtext();
void killprocess();
pid_t parsepid();
void sendkill();
void display();
void clearmessage();


void initialize(){

    state.scrn = initscr();
    
    state.header.numrows = NUMHEADROWS;
    state.currow = state.header.numrows;
    state.curline = 0;
    state.paused = 0;
    state.outputstale = 1;
    openlog();
    
    noecho();
    cbreak();
    curs_set(0);
    clear();
    keypad(stdscr, TRUE);
}

void quit(int status){
    endwin();
    closelog();
    exit(status);
}


void boldline(){
    attron(A_BOLD);
    mvaddstr(state.currow, 0, state.lineoutput[state.curline]);
    attroff(A_BOLD);
    move(state.currow, 0);
}

void writestats(){
    int i;
    char * cptr;
    char tokenline[MAXCOL];
    int row;
    char outline[MAXCOL];
    char interface[8];
    char bytesin[8];
    char bytesout[8];
    int outputrow;

    outputrow = 2; /* skip first three rows of cat output */
    row = 2; /* skip to second row on screen */

    for ( ; row < NUMHEADROWS ; row ++, outputrow ++){
	if (state.header.netdevoutput[outputrow][0] == '\0') break;
	strncpy(tokenline, state.header.netdevoutput[outputrow], MAXCOL);

	/* write interface */
	cptr = strtok(tokenline, " \t\n");
	strncpy(interface, cptr, 8);

	/* write received bytes */
	cptr = strtok(NULL, " \t\n");
	strncpy(bytesin, cptr, 8);

	/* skip other statistics */
	for (i = 0 ; i < 8 ; i ++) cptr = strtok(NULL,  " \t\n");

	/* write received bytes */
	strncpy(bytesout, cptr, 8);    

	/* build string */
	snprintf(outline,MAXCOL,"%s\t\t%s\t\t%s", interface,bytesin, bytesout);

	mvaddstr(row, 0, outline);

    }
}

void writeheader(){
    attron(A_STANDOUT);
    mvaddstr(0, 0, "netmonitor v1.0");
    attroff(A_STANDOUT);
    if (state.paused == 1)
	mvaddstr(0, 15, " - paused");
    
    mvaddstr(1, 0, "Interface\tBytes Rx\tBytes Tx");
    writestats();
    mvaddstr(state.header.numrows - 1, 0, state.header.message);
}

void clearmessage(){
    int i;

    state.header.message[0] = '\0';
    for (i = 0 ; i < COLS ; i ++){
	mvaddch(state.header.numrows - 1, i, ' ');

    }
    refresh();
}

void writemessage( char * message){
    strncpy(state.header.message, message, MAXCOL);
    mvaddstr(4, 0, message);
    refresh();
}


pid_t parsepid(){
    char tokenline[MAXCOL];
    char pidname[30];
    char pid[10];
    /* char name[20]; */
    char * cptr;
    
    strncpy(tokenline, state.lineoutput[state.curline], MAXCOL);    
    cptr = strtok(tokenline, " \t\n");    
    while (cptr != NULL){
	strncpy(pidname, cptr, 25);
	cptr = strtok(NULL, " \t\n");	
    }

    cptr = strtok(pidname, "/");
    strncpy(pid, cptr, 10);

    if (strcmp(pid, "/") == 0) return (pid_t) 0;

    return (pid_t) atoi(pid);
}

/* checks if state indicates a PID for a process selected for kill; if PID != 0
   then send SIGKILL and reset state.pidkill to 0 */
void sendkill(){
    int checkerror;

    
    /* verify the state contains a 'current' PID to kill */
    if (state.pidkill == 0){
	writelog(LOGERROR,
		 "User sent kill, but no target PID stored in state\n");
	return;
    }
    /* check if managed by systemd */
    if ( ismanaged(state.pidkill) ){
	checkerror = stopservice(state.pidkill);
	/* if (checkerror < 0) */
	    /* handle error */
	switch(checkerror)
	{
	case ERRSYSDSTOP:
	    snprintf(state.logbuffer, SIZELOGBUF,
		 "systemd failed to stop service with PID %u", state.pidkill);
	    writelog(LOGERROR, state.logbuffer);
	    break;
	case ERRSYSDNAME:

	    break;

	default:
	    snprintf(state.logbuffer, SIZELOGBUF,
		 "stopped systemd managed service with PID %u", state.pidkill);
	    writelog(LOGEVENT, state.logbuffer);
	    break;
	    
	}		
    }
		
    /* if not managed, send SIGKILL */
    else {
	snprintf(state.logbuffer, SIZELOGBUF,
		 "Killing process %u using SIGKILL", state.pidkill);
	writelog(LOGEVENT, state.logbuffer);	
	kill(state.pidkill, SIGKILL);
	
    }
    state.pidkill = 0;
    clearmessage();
    runnetstat();
    display();
}

void killprocess(){    
    pid_t pid;
    char message [100];

    pid = parsepid();

    if (pid == 0){
	writemessage("Cannot kill this process - rerun program as root");
    }

    else{
	snprintf(message, 100, "Kill process PID %u? (Y/n)", pid);
	writemessage(message);
	state.pidkill = pid;
    }
}

void writelines(){
    int row;
    int linenum;
    int startline;

    startline = state.curline - state.currow + state.header.numrows;
    if (startline < 0) startline = 0;
    
    for (row = state.header.numrows, linenum = startline ;
	 linenum < state.numlines ; row ++, linenum ++){
	mvaddstr(row, 0, state.lineoutput[linenum]);
	/* if (row == LINES) break; */
    }
}


void display(){

    clear();
    writeheader();
    writelines();
    boldline();
    refresh();
}

void readnetdev(){
    FILE * pipe;
    char line[MAXCOL];
    int row;
    char * tmp;

    pipe = popen("cat /proc/net/dev 2>/dev/null", "r");
    
    for (row = 0 ; row < NUMHEADROWS + 2 ; row ++){
	tmp = fgets(line, MAXCOL, pipe);
	if (tmp == NULL) break;
	strncpy(state.header.netdevoutput[row], line, COLS);
	state.header.netdevoutput[row][MAXCOL-1] = 0; /* remove \n */
	                                              /* add null terminator */
    }

    pclose(pipe);    
}


static void * netstatthreadf(void *arg){
    int checkerror;

    checkerror = pthread_detach(pthread_self());
    if (checkerror){
	perror("Error detatching thread");
    }


    //writelines();
    runnetstat();
    pthread_mutex_lock(&netstatmtx);
    state.outputstale = 0;
    pthread_mutex_unlock(&netstatmtx);

    pthread_exit(NULL);
    return NULL;
}

void runnetstat(){
    FILE * pipe;
    char line[MAXCOL];
    int row;
    char * tmp;

    pipe = popen("netstat -a -n -p --protocol inet 2>/dev/null", "r");
    
    for (row = 0 ; row < MAXROW ; row ++){
	tmp = fgets(line, MAXCOL, pipe);
	if (tmp == NULL) break;
	strncpy(state.lineoutput[row], line, COLS);
	state.lineoutput[row][MAXCOL-1] = 0; /* remove newline character */
	                                     /* add null terminator */
    }
    state.numlines = row;
    if (state.curline >= state.numlines) 
	state.curline = state.numlines - 1;

    if (state.currow >= row + state.header.numrows) 
	state.currow = row + state.header.numrows - 1;

    pclose(pipe);    
}

void movecursor(int delta){
    int newrow;
    int newline;
    
    newrow = state.currow + delta;
    newline = state.curline + delta;
    if (newline >= state.numlines || newline < 0 ) {
	return;
	move(state.currow, 0);
	quit(0);
    }    

    else if (newrow >= LINES){
	state.curline = newline;
	display();
    }
	
    else if (newrow < state.header.numrows){
	state.curline = newline;
	display();    
    }

    else{
	/* first, unbold current line */
	mvaddstr(state.currow, 0, state.lineoutput[state.curline]);	
	state.curline = newline;
	state.currow = newrow;
	boldline();
	refresh();
    }
}

void togglepause(){

    if (state.paused == 0){
	state.paused = 1;
	writeheader();
	refresh();
    }
    else if (state.paused == 1) state.paused = 0;
    else writelog(LOGERROR, "Error toggling pause\n");

}

void handleinput()
{
    char input;
 
    clearmessage();
    input = getch();

    if (input == 'q') quit(0); 
    else if (input == 'k') killprocess();
    else if (input == 2) movecursor(1);
    else if (input == 3) movecursor(-1);
    else if (input == 'Y' && state.pidkill != 0) sendkill();
    else if (input == 'p') togglepause();
    
}


int main(int argc, char** argv)
{
    fd_set mask;
    fd_set dummy_mask,temp_mask;
    struct timeval timeout;
    int numfds;

    pthread_t tid;
    int checkerror;
    
    initialize();


    readnetdev();
    checkerror = pthread_create(&tid, NULL, netstatthreadf, NULL);
    if(checkerror != 0){
	/* handle error */
	perror("Could not create thread");
	quit(-1);
    }

    //runnetstat();
    //display();
    
    FD_ZERO( &mask );
    FD_ZERO( &dummy_mask );
    FD_SET( STDIN_FILENO, &mask );

    timeout.tv_sec = SELECTTIMEOUT;
    timeout.tv_usec = 0;
    
    while(1){
	temp_mask = mask;
	numfds = select(FD_SETSIZE, &temp_mask, &dummy_mask, &dummy_mask,
			&timeout);
	if (numfds > 0){
	    handleinput();
	}

	else{
	    /* reselt select timeout */
	    timeout.tv_sec = SELECTTIMEOUT;
	    timeout.tv_usec = 0;
	    if (state.paused == 1) continue;
	    //readnetdev();
	    pthread_mutex_lock(&netstatmtx);
	    /* check if an a thread has completed running netstat */
	    if (state.outputstale == 0){
		display();

		/* start new thread */
		checkerror = pthread_create(&tid, NULL, netstatthreadf, NULL);
		if(checkerror != 0){
		    /* handle error */
		    perror("Could not create thread");
		    quit(-1);
		}
		//runnetstat();	    
		/* mark as stale */
		state.outputstale = 1;		
		/* present new data */
	    }
 	    pthread_mutex_unlock(&netstatmtx);	    
	}
    }
    
    quit(0);

    /* never reached */
    return 0;
}
