/* 
 * tsh - A tiny shell program with job control
 * 
 * Dylan Kane dqk5384
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
  pid_t pid;              /* job PID */
  int jid;                /* job ID [1, 2, ...] */
  int state;              /* UNDEF, BG, FG, or ST */
  char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */


/* Function prototypes */

/* Wrapper functions */
pid_t Fork(void);
unsigned int Sleep(unsigned int secs);
void Sigfillset(sigset_t *set);
void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset);
void Sigemptyset(sigset_t *set);
void Sigaddset(sigset_t *set, int signum);

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) 
{
  char c;
  char cmdline[MAXLINE];
  int emit_prompt = 1; /* emit prompt (default) */

  /* Redirect stderr to stdout (so that driver will get all output
   * on the pipe connected to stdout) */
  dup2(1, 2);

  /* Parse the command line */
  while ((c = getopt(argc, argv, "hvp")) != EOF) {
    switch (c) {
      case 'h':             /* print help message */
        usage();
        break;
      case 'v':             /* emit additional diagnostic info */
        verbose = 1;
        break;
      case 'p':             /* don't print a prompt */
        emit_prompt = 0;  /* handy for automatic testing */
        break;
      default:
        usage();
    }
  }

  /* Install the signal handlers */

  /* These are the ones you will need to implement */
  Signal(SIGINT,  sigint_handler);   /* ctrl-c */
  Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
  Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

  /* This one provides a clean way to kill the shell */
  Signal(SIGQUIT, sigquit_handler); 

  /* Initialize the job list */
  initjobs(jobs);

  /* Execute the shell's read/eval loop */
  while (1) {

    /* Read command line */
    if (emit_prompt) {
      printf("%s", prompt);
      fflush(stdout);
    }
    if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
      app_error("fgets error");
    if (feof(stdin)) { /* End of file (ctrl-d) */
      fflush(stdout);
      exit(0);
    }

    /* Evaluate the command line */
    eval(cmdline);
    fflush(stdout);
    fflush(stdout);
  } 

  exit(0); /* control never reaches here */
}

/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
void eval(char *cmdline) 
{

  char *argv[MAXARGS];
  
  // initialize masks
  sigset_t mask;
  Sigemptyset(&mask);
  Sigaddset(&mask, SIGCHLD);

  // parse command line into array and check for "&""
  int job_bg_option = parseline(cmdline, argv);

  // check for null inputs
  if (*argv == NULL) return;

  int test_builtin_cmd = builtin_cmd(argv);

  // check if command is a built in one, if not then
  // fork and execute the command at given file path 
  if (!test_builtin_cmd) {
    
    // set mask to prevent signal handler from executing
    Sigprocmask(SIG_BLOCK, &mask, 0);
    
    // initialize pid to differentiate between parent and
    // child, as well as using "Fork" wrapper instead of
    // "fork". This custom function does error handeling,
    // can be found near bottom of the file. 
    pid_t pid;
    pid = Fork();

    // check if parent or child
    if (!pid) {
      
      // if child then change the process group to help
      // with sending signals due to it being an 
      // emulated shell
      if (setpgid(0,0) < 0) {
        printf("Process group change did not work");
      };

      // execute the command in the child
      if (execve(*argv, argv, NULL) < 0) {
        printf("%s: Command not found\n", argv[0]);
        exit(0);
      }

    } else {
      
      // now as the parent, add the job to the job list
      if (job_bg_option) {
        int add_job_status = addjob(jobs, pid, BG, cmdline);
        if (add_job_status) {
          printf("[%d] (%d) %s", pid2jid(pid), pid, cmdline);
        }
      } else {
        addjob(jobs, pid, FG, cmdline);
      }

      // unblock mask now so sigchld_handler may be executed
      Sigprocmask(SIG_UNBLOCK, &mask, 0);

      // call waitfg function to wait for FG job to finish
      // before giving control back to the user
	    waitfg(pid);
    }
  } 
  return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
  static char array[MAXLINE]; /* holds local copy of command line */
  char *buf = array;          /* ptr that traverses command line */
  char *delim;                /* points to first space delimiter */
  int argc;                   /* number of args */
  int bg;                     /* background job? */

  strcpy(buf, cmdline);
  buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
  while (*buf && (*buf == ' ')) /* ignore leading spaces */
    buf++;

  /* Build the argv list */
  argc = 0;
  if (*buf == '\'') {
    buf++;
    delim = strchr(buf, '\'');
  }
  else {
    delim = strchr(buf, ' ');
  }

  while (argc < MAXARGS-1 && delim) {
    argv[argc++] = buf;
    *delim = '\0';
    buf = delim + 1;
    while (*buf && (*buf == ' ')) /* ignore spaces */
      buf++;

    if (*buf == '\'') {
      buf++;
      delim = strchr(buf, '\'');
    }
    else {
      delim = strchr(buf, ' ');
    }
  }
  if (delim) {
    fprintf(stderr, "Too many arguments.\n");
    argc = 0; //treat it as an empty line.
  }
  argv[argc] = NULL;

  if (argc == 0)  /* ignore blank line */
    return 1;

  /* should the job run in the background? */
  if ((bg = (*argv[argc-1] == '&')) != 0) {
    argv[--argc] = NULL;
  }
  return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) 
{

  // check for each individual built in command and execute functions
  // accordingly. If argument does not match any of the commands then
  // return 0, if it does return a 1
  if (strcmp(*argv, "quit") == 0) {
    exit(0);
  } else if (strcmp(*argv, "fg") == 0) {
    do_bgfg(argv);
  } else if (strcmp(*argv, "bg") == 0) {
    do_bgfg(argv);
  } else if (strcmp(*argv, "jobs") == 0) {
    listjobs(jobs);
  } else if (strcmp(*argv, "&") == 0) {
    return 1;
  } else {
    return 0;
  }
  return 1;
	
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
  // initialize needed variables
  int pid = 0;
  int jid = 0;
  struct job_t* job_holder;

  // check the second arguments is not equal to null
  if (*(argv+1) != NULL) {

    // create variable for "%" char, use number to prevent
    // confusion in referencing variables
    char percent_symbol = 37;
    char* pid_or_jid = *(argv+1);
    
    // check if JID or PID
    if (pid_or_jid[0] != percent_symbol) {
      // convert argument to number
      pid = atoi(pid_or_jid);

      // execute if no PID
      if (!pid) {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
      }

      // create job variable to see if process exists
      job_holder = getjobpid(jobs, pid);
      if (!job_holder) {
        printf("(%d): No such process\n", pid);
        return;
      }
    } else {
      //convert argument to number
      jid = atoi(&pid_or_jid[1]);

      // execute if no JID
      if (!jid) {
        printf("%s: argument must be a PID or %%jobid\n", argv[0]);
        return;
      }

      // create job variable to see if process exists
      job_holder = getjobjid(jobs, jid);
      if (!job_holder) {
        printf("%c%d: No such job\n", percent_symbol, jid);
        return;
      }
      //assign pid to work with later
      pid = job_holder->pid;
    }

    if (!pid && !jid) {
      printf("%s: argument must be a PID or %%jobid\n", argv[0]);
    } else {

      // check state of job 
      if (job_holder->state == BG) {

        // if job is BG then convert to FG and wait for it to finish
        // before giving control back to user by calling waitfg
        job_holder->state = FG;
        waitfg(job_holder->pid);

      } else if (job_holder->state == ST) {

        // if job is ST then check if it needs to become fg or bg
        if (strcmp(argv[0], "bg") == 0) {

          // convert state to BG and send SIGCONT signal
          job_holder->state = BG; 
          printf("[%d] (%d) %s", job_holder->jid, job_holder->pid, job_holder->cmdline);
          kill(-pid, SIGCONT);
        } else if (strcmp(argv[0], "fg") == 0) {

          // convert state to FG, send SIGCONT signal and then wait 
          // for it to finish before giving control back to the user
          // with waitfg function
          job_holder->state = FG;
          kill(-pid, SIGCONT);
          waitfg(job_holder->pid);
        }
      } 
    }
  } else {
    printf("%s command requires PID or %%jobid argument\n", argv[0]);
  }

  return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
  
  // create job variable
  struct job_t* job_holder = getjobpid(jobs, pid);

  // while job's state is equal to FG wait for it to change.
  // this is for taking away users control while foreground
  // process is running.
  while ((job_holder->state) == FG) {

    // Sleep wrapper function for error handeling
    Sleep(1);
  }
	
	return;
}

/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
void sigchld_handler(int sig) 
{
  int status;
  pid_t pid;
  
  // while loop that uses waitpid with arguments -1 to use for all
  // pid, status for the state of course, and then WNOHANG | WUNTRACED
  // to all loop to work without impeeding on control from user.
  // While this waitpid is greater than zero check for signals
  while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0 ) {

    if (WIFSIGNALED(status)) {  

      // delete job if the child process is terminated because of and uncaught signal
      printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
      deletejob(jobs,pid);
    } else if (WIFSTOPPED(status)) { 

      // changed state of job to ST if child is currently stopped
      struct job_t* job_holder = getjobpid(jobs, pid);
      job_holder->state = ST;
      printf("Job [%d] (%d) stopped by signal %d\n", job_holder->jid, pid, WSTOPSIG(status));
    } else if (WIFEXITED(status)) {

      // delete job if child terminates normally
      deletejob(jobs, pid);
    }
  }
    
  // check for error from waitpid
  if (errno != ECHILD && pid < 0) {
    printf("error with waitpid %s\n", strerror(errno));
  }
    
  return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
  // get pid of FG job
  pid_t pid = fgpid(jobs);
  
  // if pid exists then send SIGINT signal to kill
  if (pid) {
	  kill(-pid, SIGINT); 
  }
    
  return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
  // get pid of FG job
  pid_t pid = fgpid(jobs);
    
  // if pid exists then send SIGTSTP signal, stop until get SIGCONT
  if (pid) {
	  kill(-pid, SIGTSTP);
  }
    
  return;
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
  job->pid = 0;
  job->jid = 0;
  job->state = UNDEF;
  job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    clearjob(&jobs[i]);
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
  int i, max=0;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid > max)
      max = jobs[i].jid;
  return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == 0) {
      jobs[i].pid = pid;
      jobs[i].state = state;
      jobs[i].jid = nextjid++;
      if (nextjid > MAXJOBS)
        nextjid = 1;
      strcpy(jobs[i].cmdline, cmdline);
      if(verbose){
        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
      }
      return 1;
    }
  }
  printf("Tried to create too many jobs\n");
  return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
  int i;

  if (pid < 1)
    return 0;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid == pid) {
      clearjob(&jobs[i]);
      nextjid = maxjid(jobs)+1;
      return 1;
    }
  }
  return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
  int i;

  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].state == FG)
      return jobs[i].pid;
  return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
  int i;

  if (pid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid)
      return &jobs[i];
  return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
  int i;

  if (jid < 1)
    return NULL;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].jid == jid)
      return &jobs[i];
  return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
  int i;

  if (pid < 1)
    return 0;
  for (i = 0; i < MAXJOBS; i++)
    if (jobs[i].pid == pid) {
      return jobs[i].jid;
    }
  return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
  int i;

  for (i = 0; i < MAXJOBS; i++) {
    if (jobs[i].pid != 0) {
      printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
      switch (jobs[i].state) {
        case BG: 
          printf("Running ");
          break;
        case FG: 
          printf("Foreground ");
          break;
        case ST: 
          printf("Stopped ");
          break;
        default:
          printf("listjobs: Internal error: job[%d].state=%d ", 
              i, jobs[i].state);
      }
      printf("%s", jobs[i].cmdline);
    }
  }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message
 */
void usage(void) 
{
  printf("Usage: shell [-hvp]\n");
  printf("   -h   print this message\n");
  printf("   -v   print additional diagnostic information\n");
  printf("   -p   do not emit a command prompt\n");
  exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
  fprintf(stdout, "%s: %s\n", msg, strerror(errno));
  exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
  fprintf(stdout, "%s\n", msg);
  exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
  struct sigaction action, old_action;

  action.sa_handler = handler;  
  sigemptyset(&action.sa_mask); /* block sigs of type being handled */
  action.sa_flags = SA_RESTART; /* restart syscalls if possible */

  if (sigaction(signum, &action, &old_action) < 0)
    unix_error("Signal error");
  return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
  printf("Terminating after receipt of SIGQUIT signal\n");
  exit(1);
}

/* 
 * Fork Wrapoer Error Checker from book
 */
pid_t Fork(void) {
  pid_t pid;

  if ((pid = fork()) < 0) {
    unix_error("Fork Malfunction");
  }
  return pid;
}

/*
 * Sleep wrapper from book
 */
unsigned int Sleep(unsigned int secs) {
  unsigned int rc;

  if ((rc = sleep(secs)) < 0)
	  unix_error("Sleep error");
  return rc;
}

/*
 * Sigfillset from book
 */
void Sigfillset(sigset_t *set) { 
  if (sigfillset(set) < 0)
	  unix_error("Sigfillset error");
  return;
}

/*
 * Sigprocmask from book
 */
void Sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
  if (sigprocmask(how, set, oldset) < 0)
	  unix_error("Sigprocmask error");
  return;
}

/*
 * Sigemptyset from book
 */
void Sigemptyset(sigset_t *set) {
  if (sigemptyset(set) < 0)
	  unix_error("Sigemptyset error");
  return;
}

/*
 * Sigaddset from book
 */
void Sigaddset(sigset_t *set, int signum) {
  if (sigaddset(set, signum) < 0)
	  unix_error("Sigaddset error");
  return;
}




