/*
 * "$Id: main.c,v 1.13 2003/09/05 01:14:51 jlovell Exp $"
 *
 *   Scheduler main loop for the Common UNIX Printing System (CUPS).
 *
 *   Copyright 1997-2003 by Easy Software Products, all rights reserved.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Easy Software Products and are protected by Federal
 *   copyright law.  Distribution and use rights are outlined in the file
 *   "LICENSE" which should have been included with this file.  If this
 *   file is missing or damaged please contact Easy Software Products
 *   at:
 *
 *       Attn: CUPS Licensing Information
 *       Easy Software Products
 *       44141 Airport View Drive, Suite 204
 *       Hollywood, Maryland 20636-3111 USA
 *
 *       Voice: (301) 373-9603
 *       EMail: cups-info@cups.org
 *         WWW: http://www.cups.org
 *
 * Contents:
 *
 *   main()               - Main entry for the CUPS scheduler.
 *   CatchChildSignals()  - Catch SIGCHLD signals...
 *   HoldSignals()        - Hold child and termination signals.
 *   IgnoreChildSignals() - Ignore SIGCHLD signals...
 *   ReleaseSignals()     - Release signals for delivery.
 *   SetString()          - Set a string value.
 *   SetStringf()         - Set a formatted string value.
 *   parent_handler()     - Catch USR1/CHLD signals...
 *   sigchld_handler()    - Handle 'child' signals from old processes.
 *   sighup_handler()     - Handle 'hangup' signals to reconfigure the scheduler.
 *   sigterm_handler()    - Handle 'terminate' signals that stop the scheduler.
 *   usage()              - Show scheduler usage.
 */

/*
 * Include necessary headers...
 */

#define _MAIN_C_
#include "cupsd.h"
#include <sys/resource.h>
#include <syslog.h>
#include <grp.h>

#if defined(HAVE_MALLOC_H) && defined(HAVE_MALLINFO)
#  include <malloc.h>
#endif /* HAVE_MALLOC_H && HAVE_MALLINFO */

#ifdef __APPLE__
#include <mach/mach.h>
#include <mach/mach_error.h>
#include <servers/bootstrap.h>

#ifdef HAVE_NOTIFY_H
#include <notify.h>
#endif

static kern_return_t registerBootstrapService();
static kern_return_t destroyBootstrapService();

static mach_port_t  server_priv_port;
static char service_name[] = "/usr/sbin/cupsd";

#endif	/* __APPLE__ */

/*
 * Local functions...
 */

static void	parent_handler(int sig);
static void	sigchld_handler(int sig);
static void	sighup_handler(int sig);
static void	sigterm_handler(int sig);
static void	usage(void);


/*
 * Local globals...
 */

static int	parent_signal = 0;	/* Set to signal number from child */
static int	holdcount = 0;		/* Number of times "hold" was called */
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
static sigset_t	holdmask;		/* Old POSIX signal mask */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


/*
 * 'main()' - Main entry for the CUPS scheduler.
 */

int					/* O - Exit status */
main(int  argc,				/* I - Number of command-line arguments */
     char *argv[])			/* I - Command-line arguments */
{
  int			i;		/* Looping var */
  char			*opt;		/* Option character */
  int			fg;		/* Run in the foreground */
  fd_set		*input,		/* Input set for select() */
			*output;	/* Output set for select() */
  client_t		*con;		/* Current client */
  job_t			*job,		/* Current job */
			*next;		/* Next job */
  listener_t		*lis;		/* Current listener */
  time_t		activity;	/* Activity timer */
  time_t		browse_time;	/* Next browse send time */
  time_t		senddoc_time;	/* Send-Document time */
#ifdef HAVE_MALLINFO
  time_t		mallinfo_time;	/* Malloc information time */
#endif /* HAVE_MALLINFO */
  struct timeval	timeout;	/* select() timeout */
  struct rlimit		limit;		/* Runtime limit */
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  struct sigaction	action;		/* Actions for POSIX signals */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */
#ifdef __sgi
  cups_file_t		*fp;		/* Fake lpsched lock file */
#endif /* __sgi */
#ifdef __APPLE__
  int			debug = 0;	/* Debugging mode, don't register as Mach service */
#endif


 /*
  * Check for command-line arguments...
  */

  fg = 0;

  for (i = 1; i < argc; i ++)
    if (argv[i][0] == '-')
      for (opt = argv[i] + 1; *opt != '\0'; opt ++)
        switch (*opt)
	{
	  case 'c' : /* Configuration file */
	      i ++;
	      if (i >= argc)
	        usage();

              if (argv[i][0] == '/')
	      {
	       /*
	        * Absolute directory...
		*/

		SetString(&ConfigurationFile, argv[i]);
              }
	      else
	      {
	       /*
	        * Relative directory...
		*/

                char current[1024];	/* Current directory */


                getcwd(current, sizeof(current));
		SetStringf(&ConfigurationFile, "%s/%s", current, argv[i]);
              }
	      break;

          case 'f' : /* Run in foreground... */
	      fg = 1;
	      break;

          case 'F' : /* Run in foreground, but still disconnect from terminal... */
	      fg = -1;
	      break;

#ifdef __APPLE__
          case 'd' : /* Debugging mode */
	      debug = 1;
	      break;
#endif

	  default : /* Unknown option */
              fprintf(stderr, "cupsd: Unknown option \'%c\' - aborting!\n", *opt);
	      usage();
	      break;
	}
    else
    {
      fprintf(stderr, "cupsd: Unknown argument \'%s\' - aborting!\n", argv[i]);
      usage();
    }

  if (!ConfigurationFile)
    SetString(&ConfigurationFile, CUPS_SERVERROOT "/cupsd.conf");

 /*
  * If the user hasn't specified "-f", run in the background...
  */

  if (!fg)
  {
   /*
    * Setup signal handlers for the parent...
    */

#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
    sigset(SIGUSR1, parent_handler);
    sigset(SIGCHLD, parent_handler);

    sigset(SIGHUP, SIG_IGN);
#elif defined(HAVE_SIGACTION)
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGUSR1);
    action.sa_handler = parent_handler;
    sigaction(SIGUSR1, &action, NULL);
    sigaction(SIGCHLD, &action, NULL);

    sigemptyset(&action.sa_mask);
    action.sa_handler = SIG_IGN;
    sigaction(SIGHUP, &action, NULL);
#else
    signal(SIGUSR1, parent_handler);
    signal(SIGCLD, parent_handler);

    signal(SIGHUP, SIG_IGN);
#endif /* HAVE_SIGSET */

    if (fork() > 0)
    {
     /*
      * OK, wait for the child to startup and send us SIGUSR1 or to crash
      * and the OS send us SIGCHLD...  We also need to ignore SIGHUP which
      * might be sent by the init script to restart the scheduler...
      */

      for (; parent_signal == 0;)
        sleep(1);

      if (parent_signal == SIGUSR1)
        return (0);

      if (wait(&i) < 0)
      {
        perror("cupsd");
	i = 1;
      }
      else if (i >= 256)
        fprintf(stderr, "cupsd: Child exited with status %d!\n", i / 256);
      else
        fprintf(stderr, "cupsd: Child exited on signal %d!\n", i);

      return (i);
    }
  }

  if (fg < 1)
  {
   /*
    * Make sure we aren't tying up any filesystems...
    */

    chdir("/");

#ifndef DEBUG
   /*
    * Disable core dumps...
    */

    getrlimit(RLIMIT_CORE, &limit);
    limit.rlim_cur = 0;
    setrlimit(RLIMIT_CORE, &limit);

   /*
    * Disconnect from the controlling terminal...
    */

    close(0);
    close(1);
    close(2);

    setsid();
#endif /* DEBUG */
  }

 /*
  * Set the timezone info...
  */

  if (getenv("TZ") != NULL)
    SetStringf(&TZ, "TZ=%s", getenv("TZ"));
  else
    SetString(&TZ, "");

  tzset();

#ifdef LC_TIME
  setlocale(LC_TIME, "");
#endif /* LC_TIME */

 /*
  * Set the maximum number of files...
  */

  getrlimit(RLIMIT_NOFILE, &limit);

  if (limit.rlim_max > CUPS_MAX_FDS)
    MaxFDs = CUPS_MAX_FDS;
  else
    MaxFDs = limit.rlim_max;

  limit.rlim_cur = MaxFDs;

  setrlimit(RLIMIT_NOFILE, &limit);

 /*
  * Allocate memory for the input and output sets...
  */

  SetSize   = (MaxFDs + 7) / 8;
  InputSet  = (fd_set *)calloc(1, SetSize);
  OutputSet = (fd_set *)calloc(1, SetSize);
  input     = (fd_set *)calloc(1, SetSize);
  output    = (fd_set *)calloc(1, SetSize);

  if (InputSet == NULL || OutputSet == NULL || input == NULL || output == NULL)
  {
    syslog(LOG_LPR, "Unable to allocate memory for select() sets - exiting!");
    return (1);
  }

 /*
  * Read configuration...
  */

  if (!ReadConfiguration())
  {
    syslog(LOG_LPR, "Unable to read configuration file \'%s\' - exiting!",
           ConfigurationFile);
    return (1);
  }

 /*
  * Catch hangup and child signals and ignore broken pipes...
  */

#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  if (RunAsUser)
    sigset(SIGHUP, sigterm_handler);
  else
    sigset(SIGHUP, sighup_handler);

  sigset(SIGPIPE, SIG_IGN);
  sigset(SIGTERM, sigterm_handler);
#elif defined(HAVE_SIGACTION)
  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGHUP);

  if (RunAsUser)
    action.sa_handler = sigterm_handler;
  else
    action.sa_handler = sighup_handler;

  sigaction(SIGHUP, &action, NULL);

  sigemptyset(&action.sa_mask);
  action.sa_handler = SIG_IGN;
  sigaction(SIGPIPE, &action, NULL);

  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  sigaddset(&action.sa_mask, SIGCHLD);
  action.sa_handler = sigterm_handler;
  sigaction(SIGTERM, &action, NULL);
#else
  if (RunAsUser)
    signal(SIGHUP, sigterm_handler);
  else
    signal(SIGHUP, sighup_handler);

  signal(SIGPIPE, SIG_IGN);
  signal(SIGTERM, sigterm_handler);
#endif /* HAVE_SIGSET */

#ifdef __sgi
 /*
  * Try to create a fake lpsched lock file if one is not already there.
  * Some Adobe applications need it under IRIX in order to enable
  * printing...
  */

  if ((fp = cupsFileOpen("/var/spool/lp/SCHEDLOCK", "w")) == NULL)
  {
    syslog(LOG_LPR, "Unable to create fake lpsched lock file "
                    "\"/var/spool/lp/SCHEDLOCK\"\' - %s!",
           strerror(errno));
  }
  else
  {
    fchmod(cupsFileNumber(fp), 0644);
    fchown(cupsFileNumber(fp), User, Group);

    cupsFileClose(fp);
  }
#endif /* __sgi */

 /*
  * Initialize authentication certificates...
  */

  InitCerts();

 /*
  * If we are running in the background, signal the parent process that
  * we are up and running...
  */

  if (!fg)
    kill(getppid(), SIGUSR1);

#ifdef __APPLE__
 /*
  * In an effort to make cupsd crash proof register ourselves as a 
  * Mach port server and service. If we should die unexpectedly Mach
  * will receive a port-destroyed notification and will re-launch us.
  */
  if (!debug)
    registerBootstrapService();
#endif

 /*
  * If the administrator has configured the server to run as an unpriviledged
  * user, change to that user now...
  */

  if (RunAsUser)
  {
    setgid(Group);
    setgroups(1, &Group);
    setuid(User);
  }

 /*
  * Start any pending print jobs...
  */

  CheckJobs();

 /*
  * Loop forever...
  */

  browse_time  = time(NULL);
  senddoc_time = time(NULL);

#ifdef HAVE_MALLINFO
  mallinfo_time = 0;
#endif /* HAVE_MALLINFO */

  for (;;)
  {
   /*
    * Check if we need to load the server configuration file...
    */

    if (NeedReload)
    {
      if (NumClients > 0)
      {
        for (i = NumClients, con = Clients; i > 0; i --, con ++)
	  if (con->http.state == HTTP_WAITING)
	  {
	    CloseClient(con);
	    con --;
	  }
	  else
	    con->http.keep_alive = HTTP_KEEPALIVE_OFF;

        PauseListening();
      }
      else if (!ReadConfiguration())
      {
        syslog(LOG_LPR, "Unable to read configuration file \'%s\' - exiting!",
	       ConfigurationFile);
        break;
      }
    }

   /*
    * Check for available input or ready output.  If select() returns
    * 0 or -1, something bad happened and we should exit immediately.
    *
    * Note that we at least have one listening socket open at all
    * times.
    */

    memcpy(input, InputSet, SetSize);
    memcpy(output, OutputSet, SetSize);

    for (i = NumClients, con = Clients; i > 0; i --, con ++)
      if (con->http.used > 0)
        break;

    if (i)
    {
      timeout.tv_sec  = 0;
      timeout.tv_usec = 0;
    }
    else
    {
     /*
      * If we have no pending data from a client, see when we really
      * need to wake up...
      */

      timeout.tv_sec  = 1;
      timeout.tv_usec = 0;
    }

    if ((i = select(MaxFDs, input, output, NULL, &timeout)) < 0)
    {
      char	s[16384],	/* String buffer */
		*sptr;		/* Pointer into buffer */
      int	slen;		/* Length of string buffer */


     /*
      * Got an error from select!
      */

      if (errno == EINTR)	/* Just interrupted by a signal */
        continue;

     /*
      * Log all sorts of debug info to help track down the problem.
      */

      LogMessage(L_EMERG, "select() failed - %s!", strerror(errno));

      strcpy(s, "InputSet =");
      slen = 10;
      sptr = s + 10;

      for (i = 0; i < MaxFDs; i ++)
        if (FD_ISSET(i, InputSet))
	{
          snprintf(sptr, sizeof(s) - slen, " %d", i);
	  slen += strlen(sptr);
	  sptr += strlen(sptr);
	}

      LogMessage(L_EMERG, s);

      strcpy(s, "OutputSet =");
      slen = 11;
      sptr = s + 11;

      for (i = 0; i < MaxFDs; i ++)
        if (FD_ISSET(i, OutputSet))
	{
          snprintf(sptr, sizeof(s) - slen, " %d", i);
	  slen += strlen(sptr);
	  sptr += strlen(sptr);
	}

      LogMessage(L_EMERG, s);

      for (i = 0, con = Clients; i < NumClients; i ++, con ++)
        LogMessage(L_EMERG, "Clients[%d] = %d, file = %d, state = %d",
	           i, con->http.fd, con->file, con->http.state);

      for (i = 0, lis = Listeners; i < NumListeners; i ++, lis ++)
        LogMessage(L_EMERG, "Listeners[%d] = %d", i, lis->fd);

      LogMessage(L_EMERG, "BrowseSocket = %d", BrowseSocket);

      for (job = Jobs; job != NULL; job = job->next)
        LogMessage(L_EMERG, "Jobs[%d] = %d", job->id, job->pipe);

      break;
    }

    for (i = NumListeners, lis = Listeners; i > 0; i --, lis ++)
      if (FD_ISSET(lis->fd, input))
        AcceptClient(lis);

    for (i = NumClients, con = Clients; i > 0; i --, con ++)
    {
     /*
      * Process the input buffer...
      */

      if (FD_ISSET(con->http.fd, input) || con->http.used)
        if (!ReadClient(con))
	{
	  con --;
	  continue;
	}

     /*
      * Write data as needed...
      */

      if (FD_ISSET(con->http.fd, output) &&
          (!con->pipe_pid || FD_ISSET(con->file, input)))
        if (!WriteClient(con))
	{
	  con --;
	  continue;
	}

     /*
      * Check the activity and close old clients...
      */

      activity = time(NULL) - Timeout;
      if (con->http.activity < activity && !con->pipe_pid)
      {
        LogMessage(L_DEBUG, "Closing client %d after %d seconds of inactivity...",
	           con->http.fd, Timeout);

        CloseClient(con);
        con --;
        continue;
      }
    }

   /*
    * Check for status info from job filters...
    */

    for (job = Jobs; job != NULL; job = next)
    {
      next = job->next;

      if (job->pipe && FD_ISSET(job->pipe, input))
      {
       /*
        * Clear the input bit to avoid updating the next job
	* using the same status pipe file descriptor...
	*/

        FD_CLR(job->pipe, input);

       /*
        * Read any status messages from the filters...
	*/

        UpdateJob(job);
      }
    }

   /*
    * Update CGI messages as needed...
    */

    if (CGIPipes[0] >= 0 && FD_ISSET(CGIPipes[0], input))
      UpdateCGI();

   /*
    * Update the browse list as needed...
    */

    if (Browsing && BrowseProtocols)
    {
      if (BrowseSocket >= 0 && FD_ISSET(BrowseSocket, input))
        UpdateCUPSBrowse();

      if (PollPipe >= 0 && FD_ISSET(PollPipe, input))
        UpdatePolling();

#ifdef HAVE_LIBSLP
      if ((BrowseProtocols & BROWSE_SLP) && BrowseSLPRefresh <= time(NULL))
        UpdateSLPBrowse();
#endif /* HAVE_LIBSLP */

      if (time(NULL) > browse_time)
      {
        SendBrowseList();
	browse_time = time(NULL);
      }
    }

   /*
    * Update any pending multi-file documents...
    */

    if ((time(NULL) - senddoc_time) >= 10)
    {
      CheckJobs();
      senddoc_time = time(NULL);
    }

#ifdef HAVE_MALLINFO
   /*
    * Log memory usage every minute...
    */

    if ((time(NULL) - mallinfo_time) >= 60 && LogLevel >= L_DEBUG)
    {
      struct mallinfo mem;	/* Malloc information */


      mem = mallinfo();
      LogMessage(L_DEBUG, "mallinfo: arena = %d, used = %d, free = %d\n",
                 mem.arena, mem.usmblks + mem.uordblks,
		 mem.fsmblks + mem.fordblks);
      mallinfo_time = time(NULL);
    }
#endif /* HAVE_MALLINFO */

   /*
    * Update the root certificate once every 5 minutes...
    */

    if ((time(NULL) - RootCertTime) >= RootCertDuration && RootCertDuration)
    {
     /*
      * Update the root certificate...
      */

      DeleteCert(0);
      AddCert(0, "root");
    }
  }

 /*
  * If we get here something very bad happened and we need to exit
  * immediately.
  */

  DeleteAllCerts();
  CloseAllClients();
  StopListening();

#ifdef __APPLE__
  /* Because Mach will re-launch us immediately and we want to avoid
   * tight launch/die loops we sleep here for a second. Since this
   * isn't the normal exit we should never see this.
   */
  sleep(1);
#endif	/* __APPLE__ */

  return (1);
}


/*
 * 'CatchChildSignals()' - Catch SIGCHLD signals...
 */

void
CatchChildSignals(void)
{
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  struct sigaction	action;		/* Actions for POSIX signals */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGCHLD, sigchld_handler);
#elif defined(HAVE_SIGACTION)
  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGTERM);
  sigaddset(&action.sa_mask, SIGCHLD);
  action.sa_handler = sigchld_handler;
  sigaction(SIGCHLD, &action, NULL);
#else
  signal(SIGCLD, sigchld_handler);	/* No, SIGCLD isn't a typo... */
#endif /* HAVE_SIGSET */
}


/*
 * 'ClearString()' - Clear a string.
 */

void
ClearString(char **s)			/* O - String value */
{
  if (s && *s)
  {
    free(*s);
    *s = NULL;
  }
}


/*
 * 'HoldSignals()' - Hold child and termination signals.
 */

void
HoldSignals(void)
{
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  sigset_t		newmask;	/* New POSIX signal mask */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


  holdcount ++;
  if (holdcount > 1)
    return;

#ifdef HAVE_SIGSET
  sighold(SIGTERM);
  sighold(SIGCHLD);
#elif defined(HAVE_SIGACTION)
  sigemptyset(&newmask);
  sigaddset(&newmask, SIGTERM);
  sigaddset(&newmask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &newmask, &holdmask);
#endif /* HAVE_SIGSET */
}


/*
 * 'IgnoreChildSignals()' - Ignore SIGCHLD signals...
 *
 * We don't really ignore them, we set the signal handler to SIG_DFL,
 * since some OS's rely on signals for the wait4() function to work.
 */

void
IgnoreChildSignals(void)
{
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  struct sigaction	action;		/* Actions for POSIX signals */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
  sigset(SIGCHLD, SIG_DFL);
#elif defined(HAVE_SIGACTION)
  memset(&action, 0, sizeof(action));

  sigemptyset(&action.sa_mask);
  sigaddset(&action.sa_mask, SIGCHLD);
  action.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &action, NULL);
#else
  signal(SIGCLD, SIG_DFL);	/* No, SIGCLD isn't a typo... */
#endif /* HAVE_SIGSET */
}


/*
 * 'ReleaseSignals()' - Release signals for delivery.
 */

void
ReleaseSignals(void)
{
  holdcount --;
  if (holdcount > 0)
    return;

#ifdef HAVE_SIGSET
  sigrelse(SIGTERM);
  sigrelse(SIGCHLD);
#elif defined(HAVE_SIGACTION)
  sigprocmask(SIG_SETMASK, &holdmask, NULL);
#endif /* HAVE_SIGSET */
}


/*
 * 'SetString()' - Set a string value.
 */

void
SetString(char       **s,		/* O - New string */
          const char *v)		/* I - String value */
{
  if (!s || *s == v)
    return;

  if (*s)
    free(*s);

  if (v)
    *s = strdup(v);
  else
    *s = NULL;
}


/*
 * 'SetStringf()' - Set a formatted string value.
 */

void
SetStringf(char       **s,		/* O - New string */
           const char *f,		/* I - Printf-style format string */
	   ...)				/* I - Additional args as needed */
{
  char		v[1024];		/* Formatting string value */
  va_list	ap;			/* Argument pointer */
  char		*olds;			/* Old string */


  if (!s)
    return;

  olds = *s;

  if (f)
  {
    va_start(ap, f);
    vsnprintf(v, sizeof(v), f, ap);
    va_end(ap);

    *s = strdup(v);
  }
  else
    *s = NULL;

  if (olds)
    free(olds);
}


/*
 * 'parent_handler()' - Catch USR1/CHLD signals...
 */

static void
parent_handler(int sig)		/* I - Signal */
{
 /*
  * Store the signal we got from the OS and return...
  */

  parent_signal = sig;
}


/*
 * 'sigchld_handler()' - Handle 'child' signals from old processes.
 */

static void
sigchld_handler(int sig)	/* I - Signal number */
{
  int		olderrno;	/* Old errno value */
  int		status;		/* Exit status of child */
  int		pid;		/* Process ID of child */
  job_t		*job;		/* Current job */
  int		i;		/* Looping var */


  (void)sig;

 /*
  * Bump the signal count...
  */

  SignalCount ++;

 /*
  * Save the original error value (wait might overwrite it...)
  */

  olderrno = errno;

#ifdef HAVE_WAITPID
  while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
#elif defined(HAVE_WAIT3)
  while ((pid = wait3(&status, WNOHANG, NULL)) > 0)
#else
  if ((pid = wait(&status)) > 0)
#endif /* HAVE_WAITPID */
  {
    DEBUG_printf(("sigchld_handler: pid = %d, status = %d\n", pid, status));

   /*
    * Ignore SIGTERM errors - that comes when a job is cancelled...
    */

    if (status == SIGTERM)
      status = 0;

    if (status)
    {
      if (status < 256)
	LogMessage(L_ERROR, "PID %d crashed on signal %d!", pid, status);
      else
	LogMessage(L_ERROR, "PID %d stopped with status %d!", pid,
	           status / 256);

      if (LogLevel < L_DEBUG)
        LogMessage(L_INFO, "Hint: Try setting the LogLevel to \"debug\" to find out more.");
    }
    else
      LogMessage(L_DEBUG2, "PID %d exited with no errors.", pid);

   /*
    * Delete certificates for CGI processes...
    */

    if (pid)
      DeleteCert(pid);

   /*
    * Lookup the PID in the jobs list...
    */

    for (job = Jobs; job != NULL; job = job->next)
      if (job->state != NULL &&
          job->state->values[0].integer == IPP_JOB_PROCESSING)
      {
	for (i = 0; job->procs[i]; i ++)
          if (job->procs[i] == pid)
	    break;

	if (job->procs[i])
	{
	 /*
          * OK, this process has gone away; what's left?
	  */

          job->procs[i] = -pid;

          if (status && job->status >= 0)
	  {
	   /*
	    * An error occurred; save the exit status so we know to stop
	    * the printer or cancel the job when all of the filters finish...
	    *
	    * A negative status indicates that the backend failed and the
	    * printer needs to be stopped.
	    */

            if (!job->procs[i + 1])
 	      job->status = -status;	/* Backend failed */
	    else
 	      job->status = status;	/* Filter failed */
	  }
	  break;
	}
      }
  }

 /*
  * Restore the original error value...
  */

  errno = olderrno;

#ifdef HAVE_SIGSET
  sigset(SIGCHLD, sigchld_handler);
#elif !defined(HAVE_SIGACTION)
  signal(SIGCLD, sigchld_handler);
#endif /* HAVE_SIGSET */

 /*
  * Restore the signal count...
  */

  SignalCount --;
}


/*
 * 'sighup_handler()' - Handle 'hangup' signals to reconfigure the scheduler.
 */

static void
sighup_handler(int sig)	/* I - Signal number */
{
  (void)sig;

  NeedReload = RELOAD_ALL;

#ifdef HAVE_SIGSET
  sigset(SIGHUP, sighup_handler);
#elif !defined(HAVE_SIGACTION)
  signal(SIGHUP, sighup_handler);
#endif /* HAVE_SIGSET */
}


/*
 * 'sigterm_handler()' - Handle 'terminate' signals that stop the scheduler.
 */

static void
sigterm_handler(int sig)		/* I - Signal */
{
#ifdef __sgi
  struct stat	statbuf;		/* Needed for checking lpsched FIFO */
#endif /* __sgi */


  (void)sig;	/* remove compiler warnings... */

#ifdef __APPLE__
  /*
   * Unregister our service so Mach won't launch us again.
   */
  destroyBootstrapService();
#endif

 /*
  * Bump the signal count...
  */

  SignalCount ++;

 /*
  * Log an error...
  */

  LogMessage(L_ERROR, "Scheduler shutting down due to SIGTERM.");

 /*
  * Close all network clients and stop all jobs...
  */

  StopServer();

  StopAllJobs();

#ifdef HAVE_NOTIFY_POST
  /*
   * Even if notifications are paused send one last one as the server shuts down.
   */
  notify_post("com.apple.printerListChange");
#endif        /* HAVE_NOTIFY_POST */

#ifdef __sgi
 /*
  * Remove the fake IRIX lpsched lock file, but only if the existing
  * file is not a FIFO which indicates that the real IRIX lpsched is
  * running...
  */

  if (!stat("/var/spool/lp/FIFO", &statbuf))
    if (!S_ISFIFO(statbuf.st_mode))
      unlink("/var/spool/lp/SCHEDLOCK");
#endif /* __sgi */

  exit(1);
}


/*
 * 'usage()' - Show scheduler usage.
 */

static void
usage(void)
{
#ifdef __APPLE__
  fputs("Usage: cupsd [-c config-file] [-f] [-F] [-d]\n", stderr);
#else
  fputs("Usage: cupsd [-c config-file] [-f] [-F]\n", stderr);
#endif
  exit(1);
}


#ifdef __APPLE__

/*
 * 'registerBootstrapService()' - Register ourselves as a Mach port server and service.
 *
 * If we should die unexpectedly Mach will receive a port-destroyed notification and 
 * will re-launch us.
 */

static kern_return_t registerBootstrapService()
{
  kern_return_t status;
  mach_port_t service_send_port, service_rcv_port;

  /* syslog(LOG_ERR, "Registering Bootstrap Service"); */

  /*
   * See if our service name is already registered and if we have privilege to check in.
   */
  status = bootstrap_check_in(bootstrap_port, (char*)service_name, &service_rcv_port);
  if (status == KERN_SUCCESS)
  {
    /*
     * If so, we must be a followup instance of an already defined server.  In that case,
     * the bootstrap port we inherited from our parent is the server's privilege port, so set
     * that in case we have to unregister later (which requires the privilege port).
     */
    server_priv_port = bootstrap_port;
  }
  else if (status == BOOTSTRAP_UNKNOWN_SERVICE)
  {
    /* relaunch immediately, not on demand */
    status = bootstrap_create_server(bootstrap_port, "/usr/sbin/cupsd -f", getuid(),
          FALSE , &server_priv_port);
    if (status != KERN_SUCCESS)
      return status;

    status = bootstrap_create_service(server_priv_port, (char*)service_name, &service_send_port);
    if (status != KERN_SUCCESS)
    {
      mach_port_deallocate(mach_task_self(), server_priv_port);
      return status;
    }

    status = bootstrap_check_in(server_priv_port, (char*)service_name, &service_rcv_port);
    if (status != KERN_SUCCESS)
    {
      mach_port_deallocate(mach_task_self(), server_priv_port);
      mach_port_deallocate(mach_task_self(), service_send_port);
      return status;
    }
  }

  /*
   * We have no intention of responding to requests on the service port.  We are not otherwise
   * a Mach port-based service.  We are just using this mechanism for relaunch facilities.
   * So, we can dispose of all the rights we have for the service port.  We don't destroy the
   * send right for the server's privileged bootstrap port - in case we have to unregister later.
   */
  mach_port_destroy(mach_task_self(), service_rcv_port);

  return status;
}


/*
 * 'destroyBootstrapService()' - Unregister ourselves as a Mach port service.
 */

static kern_return_t destroyBootstrapService()
{
  /* syslog(LOG_ERR, "Destroying Bootstrap Service"); */
  return bootstrap_register(server_priv_port, (char*)service_name, MACH_PORT_NULL);
}

#endif	/* __APPLE__ */

/*
 * End of "$Id: main.c,v 1.13 2003/09/05 01:14:51 jlovell Exp $".
 */