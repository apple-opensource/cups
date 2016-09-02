/*
 * "$Id: job.c,v 1.5.2.2 2002/09/10 05:56:38 jlovell Exp $"
 *
 *   Job management routines for the Common UNIX Printing System (CUPS).
 *
 *   Copyright 1997-2002 by Easy Software Products, all rights reserved.
 *
 *   These coded instructions, statements, and computer programs are the
 *   property of Easy Software Products and are protected by Federal
 *   copyright law.  Distribution and use rights are outlined in the file
 *   "LICENSE.txt" which should have been included with this file.  If this
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
 *   AddJob()             - Add a new job to the job queue...
 *   CancelJob()          - Cancel the specified print job.
 *   CancelJobs()         - Cancel all jobs on the given printer or class.
 *   CheckJobs()          - Check the pending jobs and start any if the
 *                          destination is available.
 *   CleanJobs()          - Clean out old jobs.
 *   FreeAllJobs()        - Free all jobs from memory.
 *   FindJob()            - Find the specified job.
 *   GetPrinterJobCount() - Get the number of pending, processing,
 *                          or held jobs in a printer or class.
 *   GetUserJobCount()    - Get the number of pending, processing,
 *                          or held jobs for a user.
 *   HoldJob()            - Hold the specified job.
 *   LoadAllJobs()        - Load all jobs from disk.
 *   LoadJob()            - Load a job from disk.
 *   MoveJob()            - Move the specified job to a different
 *                          destination.
 *   ReleaseJob()         - Release the specified job.
 *   RestartJob()         - Restart the specified job.
 *   SaveJob()            - Save a job to disk.
 *   SetJobHoldUntil()    - Set the hold time for a job...
 *   SetJobPriority()     - Set the priority of a job, moving it up/down
 *                          in the list as needed.
 *   StartJob()           - Start a print job.
 *   StopAllJobs()        - Stop all print jobs.
 *   StopJob()            - Stop a print job.
 *   UpdateJob()          - Read a status update from a job's filters.
 *   ipp_read_file()      - Read an IPP request from a file.
 *   ipp_write_file()     - Write an IPP request to a file.
 *   start_process()      - Start a background process.
 */

/*
 * Include necessary headers...
 */

#include "cupsd.h"
#include <grp.h>


/*
 * Local functions...
 */

static ipp_state_t	ipp_read_file(const char *filename, ipp_t *ipp);
static ipp_state_t	ipp_write_file(const char *filename, ipp_t *ipp);
static void		set_time(job_t *job, const char *name);
static int		start_process(const char *command, char *argv[],
			              char *envp[], int in, int out, int err,
				      int root, int *pid);


/*
 * 'AddJob()' - Add a new job to the job queue...
 */

job_t *				/* O - New job record */
AddJob(int        priority,	/* I - Job priority */
       const char *dest)	/* I - Job destination */
{
  job_t	*job,			/* New job record */
	*current,		/* Current job in queue */
	*prev;			/* Previous job in queue */


  job = calloc(sizeof(job_t), 1);

  job->id       = NextJobId ++;
  job->priority = priority;
  strlcpy(job->dest, dest, sizeof(job->dest));

  NumJobs ++;

  for (current = Jobs, prev = NULL;
       current != NULL;
       prev = current, current = current->next)
    if (job->priority > current->priority)
      break;

  job->next = current;
  if (prev != NULL)
    prev->next = job;
  else
    Jobs = job;

  return (job);
}


/*
 * 'CancelJob()' - Cancel the specified print job.
 */

void
CancelJob(int id,		/* I - Job to cancel */
          int purge)		/* I - Purge jobs? */
{
  int	i;			/* Looping var */
  job_t	*current,		/* Current job */
	*prev;			/* Previous job in list */
  char	filename[1024];		/* Job filename */


  LogMessage(L_DEBUG, "CancelJob: id = %d", id);

  for (current = Jobs, prev = NULL; current != NULL; prev = current, current = current->next)
    if (current->id == id)
    {
     /*
      * Stop any processes that are working on the current...
      */

      DEBUG_puts("CancelJob: found job in list.");

      if (current->state->values[0].integer == IPP_JOB_PROCESSING)
	StopJob(current->id, 0);

      current->state->values[0].integer = IPP_JOB_CANCELLED;

      set_time(current, "time-at-completed");

     /*
      * Remove the print file for good if we aren't preserving jobs or
      * files...
      */

      current->current_file = 0;

      if (!JobHistory || !JobFiles || purge ||
          (current->dtype & CUPS_PRINTER_REMOTE))
        for (i = 1; i <= current->num_files; i ++)
	{
	  snprintf(filename, sizeof(filename), "%s/d%05d-%03d", RequestRoot,
	           current->id, i);
          unlink(filename);
	}

      if (JobHistory && !purge && !(current->dtype & CUPS_PRINTER_REMOTE))
      {
       /*
        * Save job state info...
	*/

        SaveJob(current->id);
      }
      else
      {
       /*
        * Remove the job info file...
	*/

	snprintf(filename, sizeof(filename), "%s/c%05d", RequestRoot,
	         current->id);
	unlink(filename);

       /*
        * Update pointers if we aren't preserving jobs...
        */

        if (prev == NULL)
          Jobs = current->next;
        else
          prev->next = current->next;

       /*
        * Free all memory used...
        */

        if (current->attrs != NULL)
          ippDelete(current->attrs);

        free(current->filetypes);

        free(current);

	NumJobs --;
      }

      return;
    }
}


/*
 * 'CancelJobs()' - Cancel all jobs on the given printer or class.
 */

void
CancelJobs(const char *dest)	/* I - Destination to cancel */
{
  job_t	*current;		/* Current job */


  for (current = Jobs; current != NULL;)
    if (strcmp(current->dest, dest) == 0)
    {
     /*
      * Cancel all jobs matching this destination...
      */

      CancelJob(current->id, 1);

      current = Jobs;
    }
    else
      current = current->next;

  CheckJobs();
}


/*
 * 'CheckJobs()' - Check the pending jobs and start any if the destination
 *                 is available.
 */

void
CheckJobs(void)
{
  job_t		*current,	/* Current job in queue */
		*next;		/* Next job in queue */
  printer_t	*printer,	/* Printer destination */
		*pclass;	/* Printer class destination */


  DEBUG_puts("CheckJobs()");

  for (current = Jobs; current != NULL; current = next)
  {
   /*
    * Save next pointer in case the job is cancelled en-route.
    */

    next = current->next;

   /*
    * Start held jobs if they are ready...
    */

    if (current->state->values[0].integer == IPP_JOB_HELD &&
        current->hold_until &&
	current->hold_until < time(NULL))
      current->state->values[0].integer = IPP_JOB_PENDING;

   /*
    * Start pending jobs if the destination is available...
    */

    if (current->state->values[0].integer == IPP_JOB_PENDING)
    {
      if ((pclass = FindClass(current->dest)) != NULL)
      {
       /*
        * If the class is remote, just pass it to the remote server...
	*/

        if (pclass->type & CUPS_PRINTER_REMOTE)
	  printer = pclass;
	else if (pclass->state != IPP_PRINTER_STOPPED)
	  printer = FindAvailablePrinter(current->dest);
	else
	  printer = NULL;
      }
      else
        printer = FindPrinter(current->dest);

      if (printer != NULL && (printer->type & CUPS_PRINTER_IMPLICIT))
      {
       /*
        * Handle implicit classes...
	*/

        pclass = printer;

	if (pclass->state != IPP_PRINTER_STOPPED)
	  printer = FindAvailablePrinter(current->dest);
	else
	  printer = NULL;
      }

      if (printer == NULL && pclass == NULL)
      {
       /*
        * Whoa, the printer and/or class for this destination went away;
	* cancel the job...
	*/

        LogMessage(L_WARN, "Printer/class %s has gone away; cancelling job %d!",
	           current->dest, current->id);
        CancelJob(current->id, 1);
      }
      else if (printer != NULL)
      {
       /*
        * See if the printer is available or remote and not printing a job;
	* if so, start the job...
	*/

        if (printer->state == IPP_PRINTER_IDLE ||	/* Printer is idle */
	    ((printer->type & CUPS_PRINTER_REMOTE) &&	/* Printer is remote */
	     !printer->job))				/* and not printing a job */
	  StartJob(current->id, printer);
      }
    }
  }
}


/*
 * 'CleanJobs()' - Clean out old jobs.
 */

void
CleanJobs(void)
{
  job_t	*job,		/* Current job */
	*next;		/* Next job */


  if (MaxJobs == 0)
    return;

  for (job = Jobs; job && NumJobs >= MaxJobs; job = next)
  {
    next = job->next;

    if (job->state->values[0].integer >= IPP_JOB_CANCELLED)
      CancelJob(job->id, 1);
  }
}


/*
 * 'FreeAllJobs()' - Free all jobs from memory.
 */

void
FreeAllJobs(void)
{
  job_t	*job,		/* Current job */
	*next;		/* Next job */


  StopAllJobs();

  for (job = Jobs; job; job = next)
  {
    next = job->next;

    ippDelete(job->attrs);
    free(job->filetypes);
    free(job);
  }

  Jobs = NULL;
}


/*
 * 'FindJob()' - Find the specified job.
 */

job_t *				/* O - Job data */
FindJob(int id)			/* I - Job ID */
{
  job_t	*current;		/* Current job */


  for (current = Jobs; current != NULL; current = current->next)
    if (current->id == id)
      break;

  return (current);
}


/*
 * 'GetPrinterJobCount()' - Get the number of pending, processing,
 *                          or held jobs in a printer or class.
 */

int					/* O - Job count */
GetPrinterJobCount(const char *dest)	/* I - Printer or class name */
{
  int	count;				/* Job count */
  job_t	*job;				/* Current job */


  for (job = Jobs, count = 0; job != NULL; job = job->next)
    if (job->state->values[0].integer <= IPP_JOB_PROCESSING &&
        strcasecmp(job->dest, dest) == 0)
      count ++;

  return (count);
}


/*
 * 'GetUserJobCount()' - Get the number of pending, processing,
 *                       or held jobs for a user.
 */

int					/* O - Job count */
GetUserJobCount(const char *username)	/* I - Username */
{
  int	count;				/* Job count */
  job_t	*job;				/* Current job */


  for (job = Jobs, count = 0; job != NULL; job = job->next)
    if (job->state->values[0].integer <= IPP_JOB_PROCESSING &&
        strcmp(job->username, username) == 0)
      count ++;

  return (count);
}


/*
 * 'HoldJob()' - Hold the specified job.
 */

void
HoldJob(int id)			/* I - Job ID */
{
  job_t	*job;			/* Job data */


  LogMessage(L_DEBUG, "HoldJob: id = %d", id);

  if ((job = FindJob(id)) == NULL)
    return;

  if (job->state->values[0].integer == IPP_JOB_PROCESSING)
    StopJob(id, 0);

  DEBUG_puts("HoldJob: setting state to held...");

  job->state->values[0].integer = IPP_JOB_HELD;

  SaveJob(id);

  CheckJobs();
}


/*
 * 'LoadAllJobs()' - Load all jobs from disk.
 */

void
LoadAllJobs(void)
{
  DIR		*dir;		/* Directory */
  DIRENT	*dent;		/* Directory entry */
  char		filename[1024];	/* Full filename of job file */
  job_t		*job,		/* New job */
		*current,	/* Current job */
		*prev;		/* Previous job */
  int		jobid,		/* Current job ID */
		fileid;		/* Current file ID */
  ipp_attribute_t *attr;	/* Job attribute */
  char		method[HTTP_MAX_URI],
				/* Method portion of URI */
		username[HTTP_MAX_URI],
				/* Username portion of URI */
		host[HTTP_MAX_URI],
				/* Host portion of URI */
		resource[HTTP_MAX_URI];
				/* Resource portion of URI */
  int		port;		/* Port portion of URI */
  printer_t	*p;		/* Printer or class */
  const char	*dest;		/* Destination */
  mime_type_t	**filetypes;	/* New filetypes array */


 /*
  * First open the requests directory...
  */

  if ((dir = opendir(RequestRoot)) == NULL)
    return;

 /*
  * Read all the c##### files...
  */

  while ((dent = readdir(dir)) != NULL)
    if (NAMLEN(dent) == 6 && dent->d_name[0] == 'c')
    {
     /*
      * Allocate memory for the job...
      */

      if ((job = calloc(sizeof(job_t), 1)) == NULL)
      {
        LogMessage(L_ERROR, "LoadAllJobs: Ran out of memory for jobs!");
	closedir(dir);
	return;
      }

      if ((job->attrs = ippNew()) == NULL)
      {
        free(job);
        LogMessage(L_ERROR, "LoadAllJobs: Ran out of memory for job attributes!");
	closedir(dir);
	return;
      }

     /*
      * Assign the job ID...
      */

      job->id = atoi(dent->d_name + 1);

      if (job->id >= NextJobId)
        NextJobId = job->id + 1;

     /*
      * Load the job control file...
      */

      snprintf(filename, sizeof(filename), "%s/%s", RequestRoot, dent->d_name);
      if (ipp_read_file(filename, job->attrs) != IPP_DATA)
      {
        LogMessage(L_ERROR, "LoadAllJobs: Unable to read job control file \"%s\"!",
	           filename);
	ippDelete(job->attrs);
	free(job);
	unlink(filename);
	continue;
      }

      job->state = ippFindAttribute(job->attrs, "job-state", IPP_TAG_ENUM);

      if ((attr = ippFindAttribute(job->attrs, "job-printer-uri", IPP_TAG_URI)) == NULL)
      {
        LogMessage(L_ERROR, "LoadAllJobs: No job-printer-uri attribute in control file \"%s\"!",
	           filename);
	ippDelete(job->attrs);
	free(job);
	unlink(filename);
	continue;
      }

      httpSeparate(attr->values[0].string.text, method, username, host,
                   &port, resource);

      if ((dest = ValidateDest(host, resource, &(job->dtype))) == NULL &&
          job->state != NULL &&
	  job->state->values[0].integer <= IPP_JOB_PROCESSING)
      {
       /*
	* Job queued on remote printer or class, so add it...
	*/

	if (strncmp(resource, "/classes/", 9) == 0)
	{
	  p = AddClass(resource + 9);
	  strcpy(p->make_model, "Remote Class on unknown");
	}
	else
	{
	  p = AddPrinter(resource + 10);
	  strcpy(p->make_model, "Remote Printer on unknown");
	}

        p->state       = IPP_PRINTER_STOPPED;
	p->type        |= CUPS_PRINTER_REMOTE;
	p->browse_time = 2147483647;

	strcpy(p->location, "Location Unknown");
	strcpy(p->info, "No Information Available");
	p->hostname[0] = '\0';

	SetPrinterAttrs(p);
	dest = p->name;
      }

      if (dest == NULL)
      {
        LogMessage(L_ERROR, "LoadAllJobs: Unable to queue job for destination \"%s\"!",
	           attr->values[0].string.text);
	ippDelete(job->attrs);
	free(job);
	unlink(filename);
	continue;
      }

      strlcpy(job->dest, dest, sizeof(job->dest));

      job->sheets     = ippFindAttribute(job->attrs, "job-media-sheets-completed",
                                         IPP_TAG_INTEGER);
      job->job_sheets = ippFindAttribute(job->attrs, "job-sheets", IPP_TAG_NAME);

      attr = ippFindAttribute(job->attrs, "job-priority", IPP_TAG_INTEGER);
      job->priority = attr->values[0].integer;

      attr = ippFindAttribute(job->attrs, "job-name", IPP_TAG_NAME);
      strlcpy(job->title, attr->values[0].string.text,
              sizeof(job->title));

      attr = ippFindAttribute(job->attrs, "job-originating-user-name", IPP_TAG_NAME);
      strlcpy(job->username, attr->values[0].string.text,
              sizeof(job->username));

     /*
      * Insert the job into the array, sorting by job priority and ID...
      */

      for (current = Jobs, prev = NULL;
           current != NULL;
	   prev = current, current = current->next)
	if (job->priority > current->priority)
	  break;
	else if (job->priority == current->priority && job->id < current->id)
	  break;

      job->next = current;
      if (prev != NULL)
	prev->next = job;
      else
	Jobs = job;

      NumJobs ++;

     /*
      * Set the job hold-until time and state...
      */

      if (job->state->values[0].integer == IPP_JOB_HELD)
      {
	if ((attr = ippFindAttribute(job->attrs, "job-hold-until", IPP_TAG_KEYWORD)) == NULL)
          attr = ippFindAttribute(job->attrs, "job-hold-until", IPP_TAG_NAME);

        if (attr == NULL)
          job->state->values[0].integer = IPP_JOB_PENDING;
	else
          SetJobHoldUntil(job->id, attr->values[0].string.text);
      }
      else if (job->state->values[0].integer == IPP_JOB_PROCESSING)
        job->state->values[0].integer = IPP_JOB_PENDING;
    }

 /*
  * Read all the d##### files...
  */

  rewinddir(dir);

  while ((dent = readdir(dir)) != NULL)
    if (NAMLEN(dent) > 7 && dent->d_name[0] == 'd')
    {
     /*
      * Find the job...
      */

      jobid  = atoi(dent->d_name + 1);
      fileid = atoi(dent->d_name + 7);

      snprintf(filename, sizeof(filename), "%s/%s", RequestRoot, dent->d_name);

      if ((job = FindJob(jobid)) == NULL)
      {
        LogMessage(L_ERROR, "LoadAllJobs: Orphaned print file \"%s\"!",
	           filename);
        unlink(filename);
	continue;
      }

      if (fileid > job->num_files)
      {
        if (job->num_files == 0)
	  filetypes = (mime_type_t **)calloc(sizeof(mime_type_t *), fileid);
	else
	  filetypes = (mime_type_t **)realloc(job->filetypes,
	                                    sizeof(mime_type_t *) * fileid);

        if (filetypes == NULL)
	{
          LogMessage(L_ERROR, "LoadAllJobs: Ran out of memory for job file types!");
	  continue;
	}

        job->filetypes = filetypes;
	job->num_files = fileid;
      }

      job->filetypes[fileid - 1] = mimeFileType(MimeDatabase, filename);

      if (job->filetypes[fileid - 1] == NULL)
        job->filetypes[fileid - 1] = mimeType(MimeDatabase, "application",
	                                      "vnd.cups-raw");
    }

  closedir(dir);

 /*
  * Clean out old jobs as needed...
  */

  CleanJobs();

 /*
  * Check to see if we need to start any jobs...
  */

  CheckJobs();
}


/*
 * 'MoveJob()' - Move the specified job to a different destination.
 */

void
MoveJob(int        id,		/* I - Job ID */
        const char *dest)	/* I - Destination */
{
  job_t			*current;/* Current job */
  ipp_attribute_t	*attr;	/* job-printer-uri attribute */
  printer_t		*p;	/* Destination printer or class */


  if ((p = FindPrinter(dest)) == NULL)
    p = FindClass(dest);

  if (p == NULL)
    return;

  for (current = Jobs; current != NULL; current = current->next)
    if (current->id == id)
    {
      if (current->state->values[0].integer >= IPP_JOB_PROCESSING)
        break;

      strlcpy(current->dest, dest, sizeof(current->dest));
      current->dtype = p->type & (CUPS_PRINTER_CLASS | CUPS_PRINTER_REMOTE);

      if ((attr = ippFindAttribute(current->attrs, "job-printer-uri", IPP_TAG_URI)) != NULL)
      {
        free(attr->values[0].string.text);
	attr->values[0].string.text = strdup(p->uri);
      }

      SaveJob(current->id);

      return;
    }
}


/*
 * 'ReleaseJob()' - Release the specified job.
 */

void
ReleaseJob(int id)		/* I - Job ID */
{
  job_t	*job;			/* Job data */


  LogMessage(L_DEBUG, "ReleaseJob: id = %d", id);

  if ((job = FindJob(id)) == NULL)
    return;

  if (job->state->values[0].integer == IPP_JOB_HELD)
  {
    DEBUG_puts("ReleaseJob: setting state to pending...");

    job->state->values[0].integer = IPP_JOB_PENDING;
    SaveJob(id);
    CheckJobs();
  }
}


/*
 * 'RestartJob()' - Restart the specified job.
 */

void
RestartJob(int id)		/* I - Job ID */
{
  job_t	*job;			/* Job data */


  if ((job = FindJob(id)) == NULL)
    return;

  if (job->state->values[0].integer == IPP_JOB_STOPPED || JobFiles)
  {
    job->state->values[0].integer = IPP_JOB_PENDING;
    SaveJob(id);
    CheckJobs();
  }
}


/*
 * 'SaveJob()' - Save a job to disk.
 */

void
SaveJob(int id)			/* I - Job ID */
{
  job_t	*job;			/* Pointer to job */
  char	filename[1024];		/* Job control filename */


  if ((job = FindJob(id)) == NULL)
    return;

  snprintf(filename, sizeof(filename), "%s/c%05d", RequestRoot, id);
  ipp_write_file(filename, job->attrs);
}


/*
 * 'SetJobHoldUntil()' - Set the hold time for a job...
 */

void
SetJobHoldUntil(int        id,		/* I - Job ID */
                const char *when)	/* I - When to resume */
{
  job_t		*job;			/* Pointer to job */
  time_t	curtime;		/* Current time */
  struct tm	*curdate;		/* Current date */
  int		hour;			/* Hold hour */
  int		minute;			/* Hold minute */
  int		second;			/* Hold second */


  LogMessage(L_DEBUG, "SetJobHoldUntil(%d, \"%s\")", id, when);

  if ((job = FindJob(id)) == NULL)
    return;

  second = 0;

  if (strcmp(when, "indefinite") == 0)
  {
   /*
    * Hold indefinitely...
    */

    job->hold_until = 0;
  }
  else if (strcmp(when, "day-time") == 0)
  {
   /*
    * Hold to 6am the next morning unless local time is < 6pm.
    */

    curtime = time(NULL);
    curdate = localtime(&curtime);

    if (curdate->tm_hour < 18)
      job->hold_until = curtime;
    else
      job->hold_until = curtime +
                        ((29 - curdate->tm_hour) * 60 + 59 -
			 curdate->tm_min) * 60 + 60 - curdate->tm_sec;
  }
  else if (strcmp(when, "evening") == 0 || strcmp(when, "night") == 0)
  {
   /*
    * Hold to 6pm unless local time is > 6pm or < 6am.
    */

    curtime = time(NULL);
    curdate = localtime(&curtime);

    if (curdate->tm_hour < 6 || curdate->tm_hour >= 18)
      job->hold_until = curtime;
    else
      job->hold_until = curtime +
                        ((17 - curdate->tm_hour) * 60 + 59 -
			 curdate->tm_min) * 60 + 60 - curdate->tm_sec;
  }  
  else if (strcmp(when, "second-shift") == 0)
  {
   /*
    * Hold to 4pm unless local time is > 4pm.
    */

    curtime = time(NULL);
    curdate = localtime(&curtime);

    if (curdate->tm_hour >= 16)
      job->hold_until = curtime;
    else
      job->hold_until = curtime +
                        ((15 - curdate->tm_hour) * 60 + 59 -
			 curdate->tm_min) * 60 + 60 - curdate->tm_sec;
  }  
  else if (strcmp(when, "third-shift") == 0)
  {
   /*
    * Hold to 12am unless local time is < 8am.
    */

    curtime = time(NULL);
    curdate = localtime(&curtime);

    if (curdate->tm_hour < 8)
      job->hold_until = curtime;
    else
      job->hold_until = curtime +
                        ((23 - curdate->tm_hour) * 60 + 59 -
			 curdate->tm_min) * 60 + 60 - curdate->tm_sec;
  }  
  else if (strcmp(when, "weekend") == 0)
  {
   /*
    * Hold to weekend unless we are in the weekend.
    */

    curtime = time(NULL);
    curdate = localtime(&curtime);

    if (curdate->tm_wday == 0 || curdate->tm_wday == 6)
      job->hold_until = curtime;
    else
      job->hold_until = curtime +
                        (((5 - curdate->tm_wday) * 24 +
                          (17 - curdate->tm_hour)) * 60 + 59 -
			   curdate->tm_min) * 60 + 60 - curdate->tm_sec;
  }  
  else if (sscanf(when, "%d:%d:%d", &hour, &minute, &second) >= 2)
  {
   /*
    * Hold to specified GMT time (HH:MM or HH:MM:SS)...
    */

    curtime = time(NULL);
    curdate = gmtime(&curtime);

    job->hold_until = curtime +
                      ((hour - curdate->tm_hour) * 60 + minute -
		       curdate->tm_min) * 60 + second - curdate->tm_sec;

   /*
    * Hold until next day as needed...
    */

    if (job->hold_until < curtime)
      job->hold_until += 24 * 60 * 60 * 60;
  }

  LogMessage(L_DEBUG, "SetJobHoldUntil: hold_until = %d", (int)job->hold_until);
}


/*
 * 'SetJobPriority()' - Set the priority of a job, moving it up/down in the
 *                      list as needed.
 */

void
SetJobPriority(int id,		/* I - Job ID */
               int priority)	/* I - New priority (0 to 100) */
{
  job_t		*job,		/* Job to change */
		*current,	/* Current job */
		*prev;		/* Previous job */
  ipp_attribute_t *attr;	/* Job attribute */


 /*
  * Find the job...
  */

  for (current = Jobs, prev = NULL;
       current != NULL;
       prev = current, current = current->next)
    if (current->id == id)
      break;

  if (current == NULL)
    return;

 /*
  * Set the new priority...
  */

  job = current;
  job->priority = priority;

  if ((attr = ippFindAttribute(job->attrs, "job-priority", IPP_TAG_INTEGER)) != NULL)
    attr->values[0].integer = priority;
  else
    ippAddInteger(job->attrs, IPP_TAG_JOB, IPP_TAG_INTEGER, "job-priority",
                  priority);

  SaveJob(job->id);

 /*
  * See if we need to do any sorting...
  */

  if ((prev == NULL || job->priority < prev->priority) &&
      (job->next == NULL || job->next->priority < job->priority))
    return;

 /*
  * Remove the job from the list, and then insert it where it belongs...
  */

  if (prev == NULL)
    Jobs = job->next;
  else
    prev->next = job->next;

  for (current = Jobs, prev = NULL;
       current != NULL;
       prev = current, current = current->next)
    if (job->priority > current->priority)
      break;

  job->next = current;
  if (prev != NULL)
    prev->next = job;
  else
    Jobs = job;
}


/*
 * 'StartJob()' - Start a print job.
 */

void
StartJob(int       id,		/* I - Job ID */
         printer_t *printer)	/* I - Printer to print job */
{
  job_t		*current;	/* Current job */
  int		i;		/* Looping var */
  int		slot;		/* Pipe slot */
  int		num_filters;	/* Number of filters for job */
  mime_filter_t	*filters;	/* Filters for job */
  char		method[255],	/* Method for output */
		*optptr;	/* Pointer to options */
  ipp_attribute_t *attr;	/* Current attribute */
  int		pid;		/* Process ID of new filter process */
  int		banner_page;	/* 1 if banner page, 0 otherwise */
  int		statusfds[2],	/* Pipes used between the filters and scheduler */
		filterfds[2][2];/* Pipes used between the filters */
  char		*argv[8],	/* Filter command-line arguments */
		filename[1024],	/* Job filename */
		command[1024],	/* Full path to filter/backend command */
		jobid[255],	/* Job ID string */
		title[IPP_MAX_NAME],
				/* Job title string */
		copies[255],	/* # copies string */
		options[16384],	/* Full list of options */
		*envp[20],	/* Environment variables */
		path[1024],	/* PATH environment variable */
		language[255],	/* LANG environment variable */
		charset[255],	/* CHARSET environment variable */
		classification[1024],
				/* CLASSIFICATION environment variable */
		content_type[1024],
				/* CONTENT_TYPE environment variable */
		device_uri[1024],
				/* DEVICE_URI environment variable */
		ppd[1024],	/* PPD environment variable */
		printer_name[255],
				/* PRINTER environment variable */
		root[1024],	/* CUPS_SERVERROOT environment variable */
		cache[255],	/* RIP_MAX_CACHE environment variable */
		tmpdir[1024],	/* TMPDIR environment variable */
		ldpath[1024],	/* LD_LIBRARY_PATH environment variable */
		nlspath[1024],	/* NLSPATH environment variable */
		datadir[1024],	/* CUPS_DATADIR environment variable */
		fontpath[1050];	/* CUPS_FONTPATH environment variable */


  LogMessage(L_DEBUG, "StartJob(%d, %p)", id, printer);

  for (current = Jobs; current != NULL; current = current->next)
    if (current->id == id)
      break;

  if (current == NULL)
    return;

  LogMessage(L_DEBUG, "StartJob() id = %d, file = %d/%d", id,
             current->current_file, current->num_files);

  if (current->num_files == 0)
  {
    LogMessage(L_ERROR, "Job ID %d has no files!  Cancelling it!", id);
    CancelJob(id, 0);
    return;
  }

 /*
  * Figure out what filters are required to convert from
  * the source to the destination type...
  */

  num_filters   = 0;
  current->cost = 0;

  if (printer->type & CUPS_PRINTER_REMOTE)
  {
   /*
    * Remote jobs go directly to the remote job...
    */

    filters = NULL;
  }
  else
  {
   /*
    * Local jobs get filtered...
    */

    filters = mimeFilter(MimeDatabase, current->filetypes[current->current_file],
                         printer->filetype, &num_filters);

    if (num_filters == 0)
    {
      LogMessage(L_ERROR, "Unable to convert file %d to printable format for job %d!",
	         current->current_file, current->id);
      current->current_file ++;

      if (current->current_file == current->num_files)
        CancelJob(current->id, 0);

      return;
    }

   /*
    * Remove NULL ("-") filters...
    */

    for (i = 0; i < num_filters;)
      if (strcmp(filters[i].filter, "-") == 0)
      {
        num_filters --;
	if (i < num_filters)
	  memcpy(filters + i, filters + i + 1,
	         (num_filters - i) * sizeof(mime_filter_t));
      }
      else
        i ++;

    if (num_filters == 0)
    {
      free(filters);
      filters = NULL;
    }
    else
    {
     /*
      * Compute filter cost...
      */

      for (i = 0; i < num_filters; i ++)
	current->cost += filters[i].cost;
    }
  }

 /*
  * See if the filter cost is too high...
  */

  if ((FilterLevel + current->cost) > FilterLimit && FilterLevel > 0 &&
      FilterLimit > 0)
  {
   /*
    * Don't print this job quite yet...
    */

    if (filters != NULL)
      free(filters);

    LogMessage(L_INFO, "Holding job %d because filter limit has been reached.",
               id);
    LogMessage(L_DEBUG, "StartJob: id = %d, file = %d, "
                        "cost = %d, level = %d, limit = %d",
               id, current->current_file, current->cost, FilterLevel,
	       FilterLimit);
    return;
  }

  FilterLevel += current->cost;

 /*
  * Update the printer and job state to "processing"...
  */

  current->state->values[0].integer = IPP_JOB_PROCESSING;
  current->status  = 0;
  current->printer = printer;
  printer->job     = current;
  SetPrinterState(printer, IPP_PRINTER_PROCESSING);

  if (current->current_file == 0)
    set_time(current, "time-at-processing");

 /*
  * Determine if we are printing a banner page or not...
  */

  if (current->job_sheets == NULL)
  {
    LogMessage(L_DEBUG, "No job-sheets attribute.");
    if ((current->job_sheets =
         ippFindAttribute(current->attrs, "job-sheets", IPP_TAG_ZERO)) != NULL)
      LogMessage(L_DEBUG, "... but someone added one without setting job_sheets!");
  }
  else if (current->job_sheets->num_values == 1)
    LogMessage(L_DEBUG, "job-sheets=%s",
               current->job_sheets->values[0].string.text);
  else
    LogMessage(L_DEBUG, "job-sheets=%s,%s",
               current->job_sheets->values[0].string.text,
               current->job_sheets->values[1].string.text);

  if (printer->type & (CUPS_PRINTER_REMOTE | CUPS_PRINTER_IMPLICIT))
    banner_page = 0;
  else if (current->job_sheets == NULL)
    banner_page = 0;
  else if (strcasecmp(current->job_sheets->values[0].string.text, "none") != 0 &&
	   current->current_file == 0)
    banner_page = 1;
  else if (current->job_sheets->num_values > 1 &&
	   strcasecmp(current->job_sheets->values[1].string.text, "none") != 0 &&
	   current->current_file == (current->num_files - 1))
    banner_page = 1;
  else
    banner_page = 0;

  LogMessage(L_DEBUG, "banner_page = %d", banner_page);

 /*
  * Building the options string is harder than it needs to be, but
  * for the moment we need to pass strings for command-line args and
  * not IPP attribute pointers... :)
  */

  optptr  = options;
  *optptr = '\0';

  snprintf(title, sizeof(title), "%s-%d", printer->name, current->id);
  strcpy(copies, "1");

  for (attr = current->attrs->attrs; attr != NULL; attr = attr->next)
  {
    if (strcmp(attr->name, "copies") == 0 &&
	attr->value_tag == IPP_TAG_INTEGER)
    {
     /*
      * Don't use the # copies attribute if we are printing the job sheets...
      */

      if (!banner_page)
        sprintf(copies, "%d", attr->values[0].integer);
    }
    else if (strcmp(attr->name, "job-name") == 0 &&
	     (attr->value_tag == IPP_TAG_NAME ||
	      attr->value_tag == IPP_TAG_NAMELANG))
      strlcpy(title, attr->values[0].string.text, sizeof(title));
    else if (attr->group_tag == IPP_TAG_JOB)
    {
     /*
      * Filter out other unwanted attributes...
      */

      if (attr->value_tag == IPP_TAG_MIMETYPE ||
	  attr->value_tag == IPP_TAG_NAMELANG ||
	  attr->value_tag == IPP_TAG_TEXTLANG ||
	  attr->value_tag == IPP_TAG_URI ||
	  attr->value_tag == IPP_TAG_URISCHEME)
	continue;

      if (strncmp(attr->name, "time-", 5) == 0)
	continue;

      if (strncmp(attr->name, "job-", 4) == 0 &&
          !(printer->type & CUPS_PRINTER_REMOTE))
	continue;

      if (strncmp(attr->name, "job-", 4) == 0 &&
          strcmp(attr->name, "job-billing") != 0 &&
          strcmp(attr->name, "job-sheets") != 0 &&
          strcmp(attr->name, "job-hold-until") != 0 &&
	  strcmp(attr->name, "job-priority") != 0)
	continue;

      if (strcmp(attr->name, "page-label") == 0 &&
	  banner_page)
        continue;

     /*
      * Otherwise add them to the list...
      */

      if (optptr > options)
	strlcat(optptr, " ", sizeof(options) - (optptr - options));

      if (attr->value_tag != IPP_TAG_BOOLEAN)
      {
	strlcat(optptr, attr->name, sizeof(options) - (optptr - options));
	strlcat(optptr, "=", sizeof(options) - (optptr - options));
      }

      for (i = 0; i < attr->num_values; i ++)
      {
	if (i)
	  strlcat(optptr, ",", sizeof(options) - (optptr - options));

	optptr += strlen(optptr);

	switch (attr->value_tag)
	{
	  case IPP_TAG_INTEGER :
	  case IPP_TAG_ENUM :
	      snprintf(optptr, sizeof(options) - (optptr - options),
	               "%d", attr->values[i].integer);
	      break;

	  case IPP_TAG_BOOLEAN :
	      if (!attr->values[i].boolean)
		strlcat(optptr, "no", sizeof(options) - (optptr - options));

	  case IPP_TAG_NOVALUE :
	      strlcat(optptr, attr->name,
	              sizeof(options) - (optptr - options));
	      break;

	  case IPP_TAG_RANGE :
	      if (attr->values[i].range.lower == attr->values[i].range.upper)
		snprintf(optptr, sizeof(options) - (optptr - options) - 1,
	        	 "%d", attr->values[i].range.lower);
              else
		snprintf(optptr, sizeof(options) - (optptr - options) - 1,
	        	 "%d-%d", attr->values[i].range.lower,
			 attr->values[i].range.upper);
	      break;

	  case IPP_TAG_RESOLUTION :
	      snprintf(optptr, sizeof(options) - (optptr - options) - 1,
	               "%dx%d%s", attr->values[i].resolution.xres,
		       attr->values[i].resolution.yres,
		       attr->values[i].resolution.units == IPP_RES_PER_INCH ?
			   "dpi" : "dpc");
	      break;

          case IPP_TAG_STRING :
	  case IPP_TAG_TEXT :
	  case IPP_TAG_NAME :
	  case IPP_TAG_KEYWORD :
	  case IPP_TAG_CHARSET :
	  case IPP_TAG_LANGUAGE :
	      if (strchr(attr->values[i].string.text, ' ') != NULL ||
		  strchr(attr->values[i].string.text, '\t') != NULL ||
		  strchr(attr->values[i].string.text, '\n') != NULL)
	      {
		strlcat(optptr, "\'", sizeof(options) - (optptr - options));
		strlcat(optptr, attr->values[i].string.text,
		        sizeof(options) - (optptr - options));
		strlcat(optptr, "\'", sizeof(options) - (optptr - options));
	      }
	      else
		strlcat(optptr, attr->values[i].string.text,
		        sizeof(options) - (optptr - options));
	      break;

          default :
	      break; /* anti-compiler-warning-code */
	}
      }

      optptr += strlen(optptr);
    }
  }

 /*
  * Build the command-line arguments for the filters.  Each filter
  * has 6 or 7 arguments:
  *
  *     argv[0] = printer
  *     argv[1] = job ID
  *     argv[2] = username
  *     argv[3] = title
  *     argv[4] = # copies
  *     argv[5] = options
  *     argv[6] = filename (optional; normally stdin)
  *
  * This allows legacy printer drivers that use the old System V
  * printing interface to be used by CUPS.
  */

  sprintf(jobid, "%d", current->id);
  snprintf(filename, sizeof(filename), "%s/d%05d-%03d", RequestRoot,
           current->id, current->current_file + 1);

  argv[0] = printer->name;
  argv[1] = jobid;
  argv[2] = current->username;
  argv[3] = title;
  argv[4] = copies;
  argv[5] = options;
  argv[6] = filename;
  argv[7] = NULL;

  LogMessage(L_DEBUG, "StartJob: argv = \"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"",
             argv[0], argv[1], argv[2], argv[3], argv[4], argv[5], argv[6]);

 /*
  * Create environment variable strings for the filters...
  */

  attr = ippFindAttribute(current->attrs, "attributes-natural-language",
                          IPP_TAG_LANGUAGE);

  switch (strlen(attr->values[0].string.text))
  {
    default :
       /*
        * This is an unknown or badly formatted language code; use
	* the POSIX locale...
	*/

	strcpy(language, "LANG=C");
	break;

    case 2 :
       /*
        * Just the language code (ll)...
	*/

        snprintf(language, sizeof(language), "LANG=%s",
	         attr->values[0].string.text);
        break;

    case 5 :
       /*
        * Language and country code (ll-cc)...
	*/

        snprintf(language, sizeof(language), "LANG=%c%c_%c%c",
	         attr->values[0].string.text[0],
		 attr->values[0].string.text[1],
		 toupper(attr->values[0].string.text[3]),
		 toupper(attr->values[0].string.text[4]));
        break;
  }

  attr = ippFindAttribute(current->attrs, "document-format",
                          IPP_TAG_MIMETYPE);
  if (attr != NULL &&
      (optptr = strstr(attr->values[0].string.text, "charset=")) != NULL)
    snprintf(charset, sizeof(charset), "CHARSET=%s", optptr + 8);
  else
  {
    attr = ippFindAttribute(current->attrs, "attributes-charset",
	                    IPP_TAG_CHARSET);
    snprintf(charset, sizeof(charset), "CHARSET=%s",
             attr->values[0].string.text);
  }

  snprintf(path, sizeof(path), "PATH=%s/filter:/bin:/usr/bin", ServerBin);
  snprintf(content_type, sizeof(content_type), "CONTENT_TYPE=%s/%s",
           current->filetypes[current->current_file]->super,
           current->filetypes[current->current_file]->type);
  snprintf(device_uri, sizeof(device_uri), "DEVICE_URI=%s", printer->device_uri);
  snprintf(ppd, sizeof(ppd), "PPD=%s/ppd/%s.ppd", ServerRoot, printer->name);
  snprintf(printer_name, sizeof(printer_name), "PRINTER=%s", printer->name);
  snprintf(cache, sizeof(cache), "RIP_MAX_CACHE=%s", RIPCache);
  snprintf(root, sizeof(root), "CUPS_SERVERROOT=%s", ServerRoot);
  snprintf(tmpdir, sizeof(tmpdir), "TMPDIR=%s", TempDir);
  snprintf(datadir, sizeof(datadir), "CUPS_DATADIR=%s", DataDir);
  snprintf(fontpath, sizeof(fontpath), "CUPS_FONTPATH=%s", FontPath);

  if (Classification[0] && !banner_page)
  {
    if ((attr = ippFindAttribute(current->attrs, "job-sheets",
                                 IPP_TAG_NAME)) == NULL)
      snprintf(classification, sizeof(classification), "CLASSIFICATION=%s",
               Classification);
    else if (attr->num_values > 1 &&
             strcmp(attr->values[1].string.text, "none") != 0)
      snprintf(classification, sizeof(classification), "CLASSIFICATION=%s",
               attr->values[1].string.text);
    else
      snprintf(classification, sizeof(classification), "CLASSIFICATION=%s",
               attr->values[0].string.text);
  }
  else
    classification[0] = '\0';

  if (getenv("LD_LIBRARY_PATH") != NULL)
    snprintf(ldpath, sizeof(ldpath), "LD_LIBRARY_PATH=%s",
             getenv("LD_LIBRARY_PATH"));
  else if (getenv("DYLD_LIBRARY_PATH") != NULL)
    snprintf(ldpath, sizeof(ldpath), "DYLD_LIBRARY_PATH=%s",
             getenv("DYLD_LIBRARY_PATH"));
  else
    ldpath[0] = '\0';

  if (getenv("NLSPATH") != NULL)
    snprintf(nlspath, sizeof(nlspath), "NLSPATH=%s", getenv("NLSPATH"));
  else
    nlspath[0] = '\0';

  envp[0]  = path;
  envp[1]  = "SOFTWARE=CUPS/1.1";
  envp[2]  = "USER=root";
  envp[3]  = charset;
  envp[4]  = language;
  envp[5]  = TZ;
  envp[6]  = ppd;
  envp[7]  = root;
  envp[8]  = cache;
  envp[9]  = tmpdir;
  envp[10] = content_type;
  envp[11] = device_uri;
  envp[12] = printer_name;
  envp[13] = datadir;
  envp[14] = fontpath;
  envp[15] = ldpath;
  envp[16] = nlspath;
  envp[17] = classification;
  envp[18] = NULL;

  LogMessage(L_DEBUG, "StartJob: envp = \"%s\",\"%s\",\"%s\",\"%s\","
                      "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\","
		      "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"",
	     envp[0], envp[1], envp[2], envp[3], envp[4],
	     envp[5], envp[6], envp[7], envp[8], envp[9],
	     envp[10], envp[11], envp[12], envp[13], envp[14],
	     envp[15], envp[16], envp[17]);

  current->current_file ++;

 /*
  * Make sure we have a buffer to read status info into...
  */

  if (current->buffer == NULL)
  {
    LogMessage(L_DEBUG2, "UpdateJob: Allocating status buffer...");

    if ((current->buffer = malloc(JOB_BUFFER_SIZE)) == NULL)
    {
      LogMessage(L_EMERG, "Unable to allocate memory for job status buffer - %s",
                 strerror(errno));
      CancelJob(current->id, 0);
      return;
    }

    current->bufused = 0;
  }

 /*
  * Now create processes for all of the filters...
  */

  if (pipe(statusfds))
  {
    LogMessage(L_ERROR, "Unable to create job status pipes - %s.",
	       strerror(errno));
    snprintf(printer->state_message, sizeof(printer->state_message),
             "Unable to create status pipes - %s.", strerror(errno));
    return;
  }

  LogMessage(L_DEBUG, "StartJob: statusfds = %d, %d",
             statusfds[0], statusfds[1]);

  current->pipe   = statusfds[0];
  current->status = 0;
  memset(current->procs, 0, sizeof(current->procs));

  filterfds[1][0] = open("/dev/null", O_RDONLY);
  filterfds[1][1] = -1;

  LogMessage(L_DEBUG, "StartJob: filterfds[%d] = %d, %d", 1, filterfds[1][0],
             filterfds[1][1]);

  for (i = 0, slot = 0; i < num_filters; i ++)
  {
    if (filters[i].filter[0] != '/')
      snprintf(command, sizeof(command), "%s/filter/%s", ServerBin,
               filters[i].filter);
    else
      strlcpy(command, filters[i].filter, sizeof(command));

    if (i < (num_filters - 1) ||
	strncmp(printer->device_uri, "file:", 5) != 0)
      pipe(filterfds[slot]);
    else
    {
      filterfds[slot][0] = -1;
      if (strncmp(printer->device_uri, "file:/dev/", 10) == 0)
	filterfds[slot][1] = open(printer->device_uri + 5,
	                          O_WRONLY | O_EXCL);
      else
	filterfds[slot][1] = open(printer->device_uri + 5,
	                          O_WRONLY | O_CREAT | O_TRUNC, 0600);
    }

    LogMessage(L_DEBUG, "StartJob: filter = \"%s\"", command);
    LogMessage(L_DEBUG, "StartJob: filterfds[%d] = %d, %d",
               slot, filterfds[slot][0], filterfds[slot][1]);

    pid = start_process(command, argv, envp, filterfds[!slot][0],
                        filterfds[slot][1], statusfds[1], 0,
			current->procs + i);

    close(filterfds[!slot][0]);
    close(filterfds[!slot][1]);

    if (pid == 0)
    {
      LogMessage(L_ERROR, "Unable to start filter \"%s\" - %s.",
                 filters[i].filter, strerror(errno));
      snprintf(printer->state_message, sizeof(printer->state_message),
               "Unable to start filter \"%s\" - %s.",
               filters[i].filter, strerror(errno));
      return;
    }

    LogMessage(L_INFO, "Started filter %s (PID %d) for job %d.",
               command, pid, current->id);

    argv[6] = NULL;
    slot    = !slot;
  }

  if (filters != NULL)
    free(filters);

 /*
  * Finally, pipe the final output into a backend process if needed...
  */

  if (strncmp(printer->device_uri, "file:", 5) != 0)
  {
    sscanf(printer->device_uri, "%254[^:]", method);
    snprintf(command, sizeof(command), "%s/backend/%s", ServerBin, method);

    argv[0] = printer->device_uri;

    filterfds[slot][0] = -1;
    filterfds[slot][1] = open("/dev/null", O_WRONLY);

    LogMessage(L_DEBUG, "StartJob: backend = \"%s\"", command);
    LogMessage(L_DEBUG, "StartJob: filterfds[%d] = %d, %d",
               slot, filterfds[slot][0], filterfds[slot][1]);

    pid = start_process(command, argv, envp, filterfds[!slot][0],
			filterfds[slot][1], statusfds[1], 1,
			current->procs + i);

    close(filterfds[!slot][0]);
    close(filterfds[!slot][1]);

    if (pid == 0)
    {
      LogMessage(L_ERROR, "Unable to start backend \"%s\" - %s.",
                 method, strerror(errno));
      snprintf(printer->state_message, sizeof(printer->state_message),
               "Unable to start backend \"%s\" - %s.", method, strerror(errno));
      return;
    }
    else
    {
      LogMessage(L_INFO, "Started backend %s (PID %d) for job %d.", command, pid,
                 current->id);
    }
  }
  else
  {
    filterfds[slot][0] = -1;
    filterfds[slot][1] = -1;

    close(filterfds[!slot][0]);
    close(filterfds[!slot][1]);
  }

  close(filterfds[slot][0]);
  close(filterfds[slot][1]);

  close(statusfds[1]);

  LogMessage(L_DEBUG2, "StartJob: Adding fd %d to InputSet...", current->pipe);

  FD_SET(current->pipe, &InputSet);
}


/*
 * 'StopAllJobs()' - Stop all print jobs.
 */

void
StopAllJobs(void)
{
  job_t	*current;		/* Current job */


  DEBUG_puts("StopAllJobs()");

  for (current = Jobs; current != NULL; current = current->next)
    if (current->state->values[0].integer == IPP_JOB_PROCESSING)
    {
      StopJob(current->id, 1);
      current->state->values[0].integer = IPP_JOB_PENDING;
    }
}


/*
 * 'StopJob()' - Stop a print job.
 */

void
StopJob(int id,			/* I - Job ID */
        int force)		/* I - 1 = Force all filters to stop */
{
  int	i;			/* Looping var */
  job_t	*current;		/* Current job */


  LogMessage(L_DEBUG, "StopJob: id = %d, force = %d", id, force);

  for (current = Jobs; current != NULL; current = current->next)
    if (current->id == id)
    {
      DEBUG_puts("StopJob: found job in list.");

      if (current->state->values[0].integer == IPP_JOB_PROCESSING)
      {
        DEBUG_puts("StopJob: job state is \'processing\'.");

        FilterLevel -= current->cost;

        if (current->status < 0)
	  SetPrinterState(current->printer, IPP_PRINTER_STOPPED);
	else if (current->printer->state != IPP_PRINTER_STOPPED)
	  SetPrinterState(current->printer, IPP_PRINTER_IDLE);

        LogMessage(L_DEBUG, "StopJob: printer state is %d", current->printer->state);

	current->state->values[0].integer = IPP_JOB_STOPPED;
        current->printer->job = NULL;
        current->printer      = NULL;

	current->current_file --;

        for (i = 0; current->procs[i]; i ++)
	  if (current->procs[i] > 0)
	  {
	    kill(current->procs[i], force ? SIGKILL : SIGTERM);
	    current->procs[i] = 0;
	  }

        if (current->pipe)
        {
	 /*
	  * Close the pipe and clear the input bit.
	  */

          LogMessage(L_DEBUG2, "StopJob: Removing fd %d from InputSet...",
	             current->pipe);

          close(current->pipe);
	  FD_CLR(current->pipe, &InputSet);
	  current->pipe = 0;
        }

        if (current->buffer)
	{
	 /*
	  * Free the status buffer...
	  */

          LogMessage(L_DEBUG2, "StopJob: Freeing status buffer...");

          free(current->buffer);
	  current->buffer  = NULL;
	  current->bufused = 0;
	}
      }
      return;
    }
}


/*
 * 'UpdateJob()' - Read a status update from a job's filters.
 */

void
UpdateJob(job_t *job)		/* I - Job to check */
{
  int		bytes;		/* Number of bytes read */
  int		copies;		/* Number of copies printed */
  char		*lineptr,	/* Pointer to end of line in buffer */
		*message;	/* Pointer to message text */
  int		loglevel;	/* Log level for message */


  if ((bytes = read(job->pipe, job->buffer + job->bufused,
                    JOB_BUFFER_SIZE - job->bufused - 1)) > 0)
  {
    job->bufused += bytes;
    job->buffer[job->bufused] = '\0';
    lineptr = strchr(job->buffer, '\n');
  }
  else if (bytes < 0 && errno == EINTR)
    return;
  else
  {
    lineptr    = job->buffer + job->bufused;
    lineptr[1] = 0;
  }

  if (job->bufused == 0 && bytes == 0)
    lineptr = NULL;

  while (lineptr != NULL)
  {
   /*
    * Terminate each line and process it...
    */

    *lineptr++ = '\0';

   /*
    * Figure out the logging level...
    */

    if (strncmp(job->buffer, "EMERG:", 6) == 0)
    {
      loglevel = L_EMERG;
      message  = job->buffer + 6;
    }
    else if (strncmp(job->buffer, "ALERT:", 6) == 0)
    {
      loglevel = L_ALERT;
      message  = job->buffer + 6;
    }
    else if (strncmp(job->buffer, "CRIT:", 5) == 0)
    {
      loglevel = L_CRIT;
      message  = job->buffer + 5;
    }
    else if (strncmp(job->buffer, "ERROR:", 6) == 0)
    {
      loglevel = L_ERROR;
      message  = job->buffer + 6;
    }
    else if (strncmp(job->buffer, "WARNING:", 8) == 0)
    {
      loglevel = L_WARN;
      message  = job->buffer + 8;
    }
    else if (strncmp(job->buffer, "NOTICE:", 6) == 0)
    {
      loglevel = L_NOTICE;
      message  = job->buffer + 6;
    }
    else if (strncmp(job->buffer, "INFO:", 5) == 0)
    {
      loglevel = L_INFO;
      message  = job->buffer + 5;
    }
    else if (strncmp(job->buffer, "DEBUG:", 6) == 0)
    {
      loglevel = L_DEBUG;
      message  = job->buffer + 6;
    }
    else if (strncmp(job->buffer, "DEBUG2:", 7) == 0)
    {
      loglevel = L_DEBUG2;
      message  = job->buffer + 7;
    }
    else if (strncmp(job->buffer, "PAGE:", 5) == 0)
    {
      loglevel = L_PAGE;
      message  = job->buffer + 5;
    }
    else
    {
      loglevel = L_DEBUG;
      message  = job->buffer;
    }

   /*
    * Skip leading whitespace in the message...
    */

    while (isspace(*message))
      message ++;

   /*
    * Send it to the log file and printer state message as needed...
    */

    if (loglevel == L_PAGE)
    {
     /*
      * Page message; send the message to the page_log file and update the
      * job sheet count...
      */

      if (job->sheets != NULL)
      {
	if (!sscanf(message, "%*d%d", &copies))
	{
          job->sheets->values[0].integer ++;

	  if (job->printer->page_limit)
	    UpdateQuota(job->printer, job->username, 1, 0);
        }
	else
	{
          job->sheets->values[0].integer += copies;

	  if (job->printer->page_limit)
	    UpdateQuota(job->printer, job->username, copies, 0);
        }
      }

      LogPage(job, message);
    }
    else
    {
     /*
      * Other status message; send it to the error_log file...
      */

      if (loglevel != L_INFO)
	LogMessage(loglevel, "%s", message);

      if ((loglevel == L_INFO && !job->status) ||
	  loglevel < L_INFO)
        strlcpy(job->printer->state_message, message,
                sizeof(job->printer->state_message));
    }

   /*
    * Copy over the buffer data we've used up...
    */

    strcpy(job->buffer, lineptr);
    job->bufused -= lineptr - job->buffer;

    if (job->bufused < 0)
      job->bufused = 0;

    lineptr = strchr(job->buffer, '\n');
  }

  if (bytes <= 0)
  {
    LogMessage(L_DEBUG, "UpdateJob: job %d, file %d is complete.",
               job->id, job->current_file - 1);

    if (job->pipe)
    {
     /*
      * Close the pipe and clear the input bit.
      */

      LogMessage(L_DEBUG2, "UpdateJob: Removing fd %d from InputSet...",
                 job->pipe);

      close(job->pipe);
      FD_CLR(job->pipe, &InputSet);
      job->pipe = 0;
    }

    if (job->status < 0)
    {
     /*
      * Backend had errors; stop it...
      */

      StopJob(job->id, 0);
      job->state->values[0].integer = IPP_JOB_PENDING;
      SaveJob(job->id);
    }
    else if (job->status > 0)
    {
     /*
      * Filter had errors; cancel it...
      */

      if (job->current_file < job->num_files)
        StartJob(job->id, job->printer);
      else
      {
        CancelJob(job->id, 0);

        if (JobHistory)
	{
          job->state->values[0].integer = IPP_JOB_ABORTED;
	  SaveJob(job->id);
	}

        CheckJobs();
      }
    }
    else
    {
     /*
      * Job printed successfully; cancel it...
      */

      if (job->current_file < job->num_files)
      {
        FilterLevel -= job->cost;
        StartJob(job->id, job->printer);
      }
      else
      {
	CancelJob(job->id, 0);

        if (JobHistory)
	{
          job->state->values[0].integer = IPP_JOB_COMPLETED;
	  SaveJob(job->id);
	}

	CheckJobs();
      }
    }
  }
}


/*
 * 'ipp_read_file()' - Read an IPP request from a file.
 */

static ipp_state_t			/* O - State */
ipp_read_file(const char *filename,	/* I - File to read from */
              ipp_t      *ipp)		/* I - Request to read into */
{
  int			fd;		/* File descriptor for file */
  int			n;		/* Length of data */
  unsigned char		buffer[8192],	/* Data buffer */
			*bufptr;	/* Pointer into buffer */
  ipp_attribute_t	*attr;		/* Current attribute */
  ipp_tag_t		tag;		/* Current tag */


 /*
  * Open the file if possible...
  */

  if (filename == NULL || ipp == NULL)
    return (IPP_ERROR);

  if ((fd = open(filename, O_RDONLY)) == -1)
    return (IPP_ERROR);

 /*
  * Read the IPP request...
  */

  ipp->state = IPP_IDLE;

  switch (ipp->state)
  {
    default :
	break; /* anti-compiler-warning-code */

    case IPP_IDLE :
        ipp->state ++; /* Avoid common problem... */

    case IPP_HEADER :
       /*
        * Get the request header...
	*/

        if ((n = read(fd, buffer, 8)) < 8)
	{
	  DEBUG_printf(("ipp_read_file: Unable to read header (%d bytes read)!\n", n));
	  close(fd);
	  return (n == 0 ? IPP_IDLE : IPP_ERROR);
	}

       /*
        * Verify the major version number...
	*/

	if (buffer[0] != 1)
	{
	  DEBUG_printf(("ipp_read_file: version number (%d.%d) is bad.\n", buffer[0],
	                buffer[1]));
	  close(fd);
	  return (IPP_ERROR);
	}

       /*
        * Then copy the request header over...
	*/

        ipp->request.any.version[0]  = buffer[0];
        ipp->request.any.version[1]  = buffer[1];
        ipp->request.any.op_status   = (buffer[2] << 8) | buffer[3];
        ipp->request.any.request_id  = (((((buffer[4] << 8) | buffer[5]) << 8) |
	                               buffer[6]) << 8) | buffer[7];

        ipp->state   = IPP_ATTRIBUTE;
	ipp->current = NULL;
	ipp->curtag  = IPP_TAG_ZERO;

    case IPP_ATTRIBUTE :
        while (read(fd, buffer, 1) > 0)
	{
	 /*
	  * Read this attribute...
	  */

          tag = (ipp_tag_t)buffer[0];

	  if (tag == IPP_TAG_END)
	  {
	   /*
	    * No more attributes left...
	    */

            DEBUG_puts("ipp_read_file: IPP_TAG_END!");

	    ipp->state = IPP_DATA;
	    break;
	  }
          else if (tag < IPP_TAG_UNSUPPORTED_VALUE)
	  {
	   /*
	    * Group tag...  Set the current group and continue...
	    */

            if (ipp->curtag == tag)
	      ippAddSeparator(ipp);

	    ipp->curtag  = tag;
	    ipp->current = NULL;
	    DEBUG_printf(("ipp_read_file: group tag = %x\n", tag));
	    continue;
	  }

          DEBUG_printf(("ipp_read_file: value tag = %x\n", tag));

         /*
	  * Get the name...
	  */

          if (read(fd, buffer, 2) < 2)
	  {
	    DEBUG_puts("ipp_read_file: unable to read name length!");
	    close(fd);
	    return (IPP_ERROR);
	  }

          n = (buffer[0] << 8) | buffer[1];

          if (n > (sizeof(buffer) - 1))
	  {
	    DEBUG_printf(("ipp_read_file: bad name length %d!\n", n));
	    return (IPP_ERROR);
	  }

          DEBUG_printf(("ipp_read_file: name length = %d\n", n));

          if (n == 0)
	  {
	   /*
	    * More values for current attribute...
	    */

            if (ipp->current == NULL)
	    {
	      close(fd);
              return (IPP_ERROR);
	    }

            attr = ipp->current;

           /*
	    * Finally, reallocate the attribute array as needed...
	    */

	    if ((attr->num_values % IPP_MAX_VALUES) == 0)
	    {
	      ipp_attribute_t	*temp,	/* Pointer to new buffer */
				*ptr;	/* Pointer in attribute list */


             /*
	      * Reallocate memory...
	      */

              if ((temp = realloc(attr, sizeof(ipp_attribute_t) +
	                                (attr->num_values + IPP_MAX_VALUES - 1) *
					sizeof(ipp_value_t))) == NULL)
	      {
		close(fd);
        	return (IPP_ERROR);
	      }

             /*
	      * Reset pointers in the list...
	      */

	      for (ptr = ipp->attrs; ptr && ptr->next != attr; ptr = ptr->next);

              if (ptr)
	        ptr->next = temp;
	      else
	        ipp->attrs = temp;

              attr = ipp->current = ipp->last = temp;
	    }
	  }
	  else
	  {
	   /*
	    * New attribute; read the name and add it...
	    */

	    if (read(fd, buffer, n) < n)
	    {
	      DEBUG_puts("ipp_read_file: unable to read name!");
	      close(fd);
	      return (IPP_ERROR);
	    }

	    buffer[n] = '\0';
	    DEBUG_printf(("ipp_read_file: name = \'%s\'\n", buffer));

	    attr = ipp->current = _ipp_add_attr(ipp, IPP_MAX_VALUES);

	    attr->group_tag  = ipp->curtag;
	    attr->value_tag  = tag;
	    attr->name       = strdup((char *)buffer);
	    attr->num_values = 0;
	  }

	  if (read(fd, buffer, 2) < 2)
	  {
	    DEBUG_puts("ipp_read_file: unable to read value length!");
	    close(fd);
	    return (IPP_ERROR);
	  }

	  n = (buffer[0] << 8) | buffer[1];
          DEBUG_printf(("ipp_read_file: value length = %d\n", n));

	  switch (tag)
	  {
	    case IPP_TAG_INTEGER :
	    case IPP_TAG_ENUM :
	        if (read(fd, buffer, 4) < 4)
	        {
	          close(fd);
                  return (IPP_ERROR);
	        }

		n = (((((buffer[0] << 8) | buffer[1]) << 8) | buffer[2]) << 8) |
		    buffer[3];

                attr->values[attr->num_values].integer = n;
	        break;
	    case IPP_TAG_BOOLEAN :
	        if (read(fd, buffer, 1) < 1)
	        {
	          close(fd);
                  return (IPP_ERROR);
	        }

                attr->values[attr->num_values].boolean = buffer[0];
	        break;
	    case IPP_TAG_TEXT :
	    case IPP_TAG_NAME :
	    case IPP_TAG_KEYWORD :
	    case IPP_TAG_STRING :
	    case IPP_TAG_URI :
	    case IPP_TAG_URISCHEME :
	    case IPP_TAG_CHARSET :
	    case IPP_TAG_LANGUAGE :
	    case IPP_TAG_MIMETYPE :
	        if (read(fd, buffer, n) < n)
	        {
	          close(fd);
                  return (IPP_ERROR);
	        }

                buffer[n] = '\0';
		DEBUG_printf(("ipp_read_file: value = \'%s\'\n", buffer));

                attr->values[attr->num_values].string.text = strdup((char *)buffer);
	        break;
	    case IPP_TAG_DATE :
	        if (read(fd, buffer, 11) < 11)
	        {
	          close(fd);
                  return (IPP_ERROR);
	        }

                memcpy(attr->values[attr->num_values].date, buffer, 11);
	        break;
	    case IPP_TAG_RESOLUTION :
	        if (read(fd, buffer, 9) < 9)
	        {
	          close(fd);
                  return (IPP_ERROR);
	        }

                attr->values[attr->num_values].resolution.xres =
		    (((((buffer[0] << 8) | buffer[1]) << 8) | buffer[2]) << 8) |
		    buffer[3];
                attr->values[attr->num_values].resolution.yres =
		    (((((buffer[4] << 8) | buffer[5]) << 8) | buffer[6]) << 8) |
		    buffer[7];
                attr->values[attr->num_values].resolution.units =
		    (ipp_res_t)buffer[8];
	        break;
	    case IPP_TAG_RANGE :
	        if (read(fd, buffer, 8) < 8)
	        {
	          close(fd);
                  return (IPP_ERROR);
	        }

                attr->values[attr->num_values].range.lower =
		    (((((buffer[0] << 8) | buffer[1]) << 8) | buffer[2]) << 8) |
		    buffer[3];
                attr->values[attr->num_values].range.upper =
		    (((((buffer[4] << 8) | buffer[5]) << 8) | buffer[6]) << 8) |
		    buffer[7];
	        break;
	    case IPP_TAG_TEXTLANG :
	    case IPP_TAG_NAMELANG :
	        if (n > sizeof(buffer))
		{
		  DEBUG_printf(("ipp_read_file: bad value length %d!\n", n));
		  return (IPP_ERROR);
		}

	        if (read(fd, buffer, n) < n)
		  return (IPP_ERROR);

                bufptr = buffer;

	       /*
	        * text-with-language and name-with-language are composite
		* values:
		*
		*    charset-length
		*    charset
		*    text-length
		*    text
		*/

		n = (bufptr[0] << 8) | bufptr[1];

                attr->values[attr->num_values].string.charset = calloc(n + 1, 1);

		memcpy(attr->values[attr->num_values].string.charset,
		       bufptr + 2, n);

                bufptr += 2 + n;
		n = (bufptr[0] << 8) | bufptr[1];

                attr->values[attr->num_values].string.text = calloc(n + 1, 1);

		memcpy(attr->values[attr->num_values].string.text,
		       bufptr + 2, n);

	        break;

            default : /* Other unsupported values */
                attr->values[attr->num_values].unknown.length = n;
	        if (n > 0)
		{
		  attr->values[attr->num_values].unknown.data = malloc(n);
	          if (read(fd, attr->values[attr->num_values].unknown.data, n) < n)
		    return (IPP_ERROR);
		}
		else
		  attr->values[attr->num_values].unknown.data = NULL;
	        break;
	  }

          attr->num_values ++;
	}
        break;

    case IPP_DATA :
        break;
  }

 /*
  * Close the file and return...
  */

  close(fd);

  return (ipp->state);
}


/*
 * 'ipp_write_file()' - Write an IPP request to a file.
 */

static ipp_state_t			/* O - State */
ipp_write_file(const char *filename,	/* I - File to write to */
               ipp_t      *ipp)		/* I - Request to write */
{
  int			fd;		/* File descriptor */
  int			i;		/* Looping var */
  int			n;		/* Length of data */
  unsigned char		buffer[8192],	/* Data buffer */
			*bufptr;	/* Pointer into buffer */
  ipp_attribute_t	*attr;		/* Current attribute */


 /*
  * Open the file if possible...
  */

  if (filename == NULL || ipp == NULL)
    return (IPP_ERROR);

  if ((fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0600)) == -1)
    return (IPP_ERROR);

  fchmod(fd, 0600);
  fchown(fd, User, Group);

 /*
  * Write the IPP request...
  */

  ipp->state = IPP_IDLE;

  switch (ipp->state)
  {
    default :
	break; /* anti-compiler-warning-code */

    case IPP_IDLE :
        ipp->state ++; /* Avoid common problem... */

    case IPP_HEADER :
       /*
        * Send the request header...
	*/

        bufptr = buffer;

	*bufptr++ = ipp->request.any.version[0];
	*bufptr++ = ipp->request.any.version[1];
	*bufptr++ = ipp->request.any.op_status >> 8;
	*bufptr++ = ipp->request.any.op_status;
	*bufptr++ = ipp->request.any.request_id >> 24;
	*bufptr++ = ipp->request.any.request_id >> 16;
	*bufptr++ = ipp->request.any.request_id >> 8;
	*bufptr++ = ipp->request.any.request_id;

        if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	{
	  DEBUG_puts("ipp_write_file: Could not write IPP header...");
	  close(fd);
	  return (IPP_ERROR);
	}

        ipp->state   = IPP_ATTRIBUTE;
	ipp->current = ipp->attrs;
	ipp->curtag  = IPP_TAG_ZERO;

    case IPP_ATTRIBUTE :
        while (ipp->current != NULL)
	{
	 /*
	  * Write this attribute...
	  */

	  bufptr = buffer;
	  attr   = ipp->current;

	  ipp->current = ipp->current->next;

          if (ipp->curtag != attr->group_tag)
	  {
	   /*
	    * Send a group operation tag...
	    */

	    ipp->curtag = attr->group_tag;

            if (attr->group_tag == IPP_TAG_ZERO)
	      continue;

            DEBUG_printf(("ipp_write_file: wrote group tag = %x\n", attr->group_tag));
	    *bufptr++ = attr->group_tag;
	  }

          if ((n = strlen(attr->name)) > (sizeof(buffer) - 3))
	    return (IPP_ERROR);

          DEBUG_printf(("ipp_write_file: writing value tag = %x\n", attr->value_tag));
          DEBUG_printf(("ipp_write_file: writing name = %d, \'%s\'\n", n, attr->name));

          *bufptr++ = attr->value_tag;
	  *bufptr++ = n >> 8;
	  *bufptr++ = n;
	  memcpy(bufptr, attr->name, n);
	  bufptr += n;

	  switch (attr->value_tag)
	  {
	    case IPP_TAG_INTEGER :
	    case IPP_TAG_ENUM :
	        for (i = 0; i < attr->num_values; i ++)
		{
                  if ((sizeof(buffer) - (bufptr - buffer)) < 9)
		  {
                    if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	            {
	              DEBUG_puts("ippWrite: Could not write IPP attribute...");
	              return (IPP_ERROR);
	            }

		    bufptr = buffer;
		  }

		  if (i)
		  {
		   /*
		    * Arrays and sets are done by sending additional
		    * values with a zero-length name...
		    */

                    *bufptr++ = attr->value_tag;
		    *bufptr++ = 0;
		    *bufptr++ = 0;
		  }

	          *bufptr++ = 0;
		  *bufptr++ = 4;
		  *bufptr++ = attr->values[i].integer >> 24;
		  *bufptr++ = attr->values[i].integer >> 16;
		  *bufptr++ = attr->values[i].integer >> 8;
		  *bufptr++ = attr->values[i].integer;
		}
		break;

	    case IPP_TAG_BOOLEAN :
	        for (i = 0; i < attr->num_values; i ++)
		{
                  if ((sizeof(buffer) - (bufptr - buffer)) < 6)
		  {
                    if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	            {
	              DEBUG_puts("ippWrite: Could not write IPP attribute...");
	              return (IPP_ERROR);
	            }

		    bufptr = buffer;
		  }

		  if (i)
		  {
		   /*
		    * Arrays and sets are done by sending additional
		    * values with a zero-length name...
		    */

                    *bufptr++ = attr->value_tag;
		    *bufptr++ = 0;
		    *bufptr++ = 0;
		  }

	          *bufptr++ = 0;
		  *bufptr++ = 1;
		  *bufptr++ = attr->values[i].boolean;
		}
		break;

	    case IPP_TAG_TEXT :
	    case IPP_TAG_NAME :
	    case IPP_TAG_KEYWORD :
	    case IPP_TAG_STRING :
	    case IPP_TAG_URI :
	    case IPP_TAG_URISCHEME :
	    case IPP_TAG_CHARSET :
	    case IPP_TAG_LANGUAGE :
	    case IPP_TAG_MIMETYPE :
	        for (i = 0; i < attr->num_values; i ++)
		{
		  if (i)
		  {
		   /*
		    * Arrays and sets are done by sending additional
		    * values with a zero-length name...
		    */

        	    DEBUG_printf(("ipp_write_file: writing value tag = %x\n",
		                  attr->value_tag));
        	    DEBUG_printf(("ipp_write_file: writing name = 0, \'\'\n"));

                    if ((sizeof(buffer) - (bufptr - buffer)) < 3)
		    {
                      if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	              {
	        	DEBUG_puts("ippWrite: Could not write IPP attribute...");
	        	return (IPP_ERROR);
	              }

		      bufptr = buffer;
		    }

                    *bufptr++ = attr->value_tag;
		    *bufptr++ = 0;
		    *bufptr++ = 0;
		  }

                  n = strlen(attr->values[i].string.text);

                  if (n > sizeof(buffer))
		    return (IPP_ERROR);

                  DEBUG_printf(("ipp_write_file: writing string = %d, \'%s\'\n", n,
		                attr->values[i].string.text));

                  if ((sizeof(buffer) - (bufptr - buffer)) < (n + 2))
		  {
                    if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	            {
	              DEBUG_puts("ipp_write_file: Could not write IPP attribute...");
	              close(fd);
	              return (IPP_ERROR);
	            }

		    bufptr = buffer;
		  }

	          *bufptr++ = n >> 8;
		  *bufptr++ = n;
		  memcpy(bufptr, attr->values[i].string.text, n);
		  bufptr += n;
		}
		break;

	    case IPP_TAG_DATE :
	        for (i = 0; i < attr->num_values; i ++)
		{
                  if ((sizeof(buffer) - (bufptr - buffer)) < 16)
		  {
                    if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	            {
	              DEBUG_puts("ippWrite: Could not write IPP attribute...");
	              return (IPP_ERROR);
	            }

		    bufptr = buffer;
		  }

		  if (i)
		  {
		   /*
		    * Arrays and sets are done by sending additional
		    * values with a zero-length name...
		    */

                    *bufptr++ = attr->value_tag;
		    *bufptr++ = 0;
		    *bufptr++ = 0;
		  }

	          *bufptr++ = 0;
		  *bufptr++ = 11;
		  memcpy(bufptr, attr->values[i].date, 11);
		  bufptr += 11;
		}
		break;

	    case IPP_TAG_RESOLUTION :
	        for (i = 0; i < attr->num_values; i ++)
		{
                  if ((sizeof(buffer) - (bufptr - buffer)) < 14)
		  {
                    if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	            {
	              DEBUG_puts("ippWrite: Could not write IPP attribute...");
	              return (IPP_ERROR);
	            }

		    bufptr = buffer;
		  }

		  if (i)
		  {
		   /*
		    * Arrays and sets are done by sending additional
		    * values with a zero-length name...
		    */

                    *bufptr++ = attr->value_tag;
		    *bufptr++ = 0;
		    *bufptr++ = 0;
		  }

	          *bufptr++ = 0;
		  *bufptr++ = 9;
		  *bufptr++ = attr->values[i].resolution.xres >> 24;
		  *bufptr++ = attr->values[i].resolution.xres >> 16;
		  *bufptr++ = attr->values[i].resolution.xres >> 8;
		  *bufptr++ = attr->values[i].resolution.xres;
		  *bufptr++ = attr->values[i].resolution.yres >> 24;
		  *bufptr++ = attr->values[i].resolution.yres >> 16;
		  *bufptr++ = attr->values[i].resolution.yres >> 8;
		  *bufptr++ = attr->values[i].resolution.yres;
		  *bufptr++ = attr->values[i].resolution.units;
		}
		break;

	    case IPP_TAG_RANGE :
	        for (i = 0; i < attr->num_values; i ++)
		{
                  if ((sizeof(buffer) - (bufptr - buffer)) < 13)
		  {
                    if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	            {
	              DEBUG_puts("ippWrite: Could not write IPP attribute...");
	              return (IPP_ERROR);
	            }

		    bufptr = buffer;
		  }

		  if (i)
		  {
		   /*
		    * Arrays and sets are done by sending additional
		    * values with a zero-length name...
		    */

                    *bufptr++ = attr->value_tag;
		    *bufptr++ = 0;
		    *bufptr++ = 0;
		  }

	          *bufptr++ = 0;
		  *bufptr++ = 8;
		  *bufptr++ = attr->values[i].range.lower >> 24;
		  *bufptr++ = attr->values[i].range.lower >> 16;
		  *bufptr++ = attr->values[i].range.lower >> 8;
		  *bufptr++ = attr->values[i].range.lower;
		  *bufptr++ = attr->values[i].range.upper >> 24;
		  *bufptr++ = attr->values[i].range.upper >> 16;
		  *bufptr++ = attr->values[i].range.upper >> 8;
		  *bufptr++ = attr->values[i].range.upper;
		}
		break;

	    case IPP_TAG_TEXTLANG :
	    case IPP_TAG_NAMELANG :
	        for (i = 0; i < attr->num_values; i ++)
		{
		  if (i)
		  {
		   /*
		    * Arrays and sets are done by sending additional
		    * values with a zero-length name...
		    */

                    if ((sizeof(buffer) - (bufptr - buffer)) < 3)
		    {
                      if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	              {
	        	DEBUG_puts("ippWrite: Could not write IPP attribute...");
	        	return (IPP_ERROR);
	              }

		      bufptr = buffer;
		    }

                    *bufptr++ = attr->value_tag;
		    *bufptr++ = 0;
		    *bufptr++ = 0;
		  }

                  n = strlen(attr->values[i].string.charset) +
		      strlen(attr->values[i].string.text) +
		      4;

                  if (n > sizeof(buffer))
		    return (IPP_ERROR);

                  if ((sizeof(buffer) - (bufptr - buffer)) < (n + 2))
		  {
                    if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	            {
	              DEBUG_puts("ipp_write_file: Could not write IPP attribute...");
	              return (IPP_ERROR);
	            }

		    bufptr = buffer;
		  }

                 /* Length of entire value */
	          *bufptr++ = n >> 8;
		  *bufptr++ = n;

                 /* Length of charset */
                  n = strlen(attr->values[i].string.charset);
	          *bufptr++ = n >> 8;
		  *bufptr++ = n;

                 /* Charset */
		  memcpy(bufptr, attr->values[i].string.charset, n);
		  bufptr += n;

                 /* Length of text */
                  n = strlen(attr->values[i].string.text);
	          *bufptr++ = n >> 8;
		  *bufptr++ = n;

                 /* Text */
		  memcpy(bufptr, attr->values[i].string.text, n);
		  bufptr += n;
		}
		break;

            default :
	        for (i = 0; i < attr->num_values; i ++)
		{
		  if (i)
		  {
		   /*
		    * Arrays and sets are done by sending additional
		    * values with a zero-length name...
		    */

                    if ((sizeof(buffer) - (bufptr - buffer)) < 3)
		    {
                      if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	              {
	        	DEBUG_puts("ippWrite: Could not write IPP attribute...");
	        	return (IPP_ERROR);
	              }

		      bufptr = buffer;
		    }

                    *bufptr++ = attr->value_tag;
		    *bufptr++ = 0;
		    *bufptr++ = 0;
		  }

                  n = attr->values[i].unknown.length;

                  if (n > sizeof(buffer))
		    return (IPP_ERROR);

                  if ((sizeof(buffer) - (bufptr - buffer)) < (n + 2))
		  {
                    if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	            {
	              DEBUG_puts("ipp_write_file: Could not write IPP attribute...");
	              return (IPP_ERROR);
	            }

		    bufptr = buffer;
		  }

                 /* Length of unknown value */
	          *bufptr++ = n >> 8;
		  *bufptr++ = n;

                 /* Value */
		  if (n > 0)
		  {
		    memcpy(bufptr, attr->values[i].unknown.data, n);
		    bufptr += n;
		  }
		}
		break;
	  }

         /*
	  * Write the data out...
	  */

          if (write(fd, (char *)buffer, bufptr - buffer) < 0)
	  {
	    DEBUG_puts("ipp_write_file: Could not write IPP attribute...");
	    close(fd);
	    return (IPP_ERROR);
	  }

          DEBUG_printf(("ipp_write_file: wrote %d bytes\n", bufptr - buffer));
	}

	if (ipp->current == NULL)
	{
         /*
	  * Done with all of the attributes; add the end-of-attributes tag...
	  */

          buffer[0] = IPP_TAG_END;
	  if (write(fd, (char *)buffer, 1) < 0)
	  {
	    DEBUG_puts("ipp_write_file: Could not write IPP end-tag...");
	    close(fd);
	    return (IPP_ERROR);
	  }

	  ipp->state = IPP_DATA;
	}
        break;

    case IPP_DATA :
        break;
  }

 /*
  * Close the file and return...
  */

  close(fd);

  return (ipp->state);
}


/*
 * 'set_time()' - Set one of the "time-at-xyz" attributes...
 */

static void
set_time(job_t      *job,	/* I - Job to update */
         const char *name)	/* I - Name of attribute */
{
  ipp_attribute_t	*attr;	/* Time attribute */


  if ((attr = ippFindAttribute(job->attrs, name, IPP_TAG_ZERO)) != NULL)
  {
    attr->value_tag         = IPP_TAG_INTEGER;
    attr->values[0].integer = time(NULL);
  }
}


/*
 * 'start_process()' - Start a background process.
 */

static int				/* O - Process ID or 0 */
start_process(const char *command,	/* I - Full path to command */
              char       *argv[],	/* I - Command-line arguments */
	      char       *envp[],	/* I - Environment */
              int        infd,		/* I - Standard input file descriptor */
	      int        outfd,		/* I - Standard output file descriptor */
	      int        errfd,		/* I - Standard error file descriptor */
	      int        root,		/* I - Run as root? */
              int        *pid)		/* O - Process ID */
{
  int	fd;				/* Looping var */
#if defined(HAVE_SIGACTION) && !defined(HAVE_SIGSET)
  sigset_t	oldmask,		/* POSIX signal masks */
		newmask;
  struct sigaction	action;		/* Actions for POSIX signals */
#endif /* HAVE_SIGACTION && !HAVE_SIGSET */


  LogMessage(L_DEBUG, "start_process(\"%s\", %p, %p, %d, %d, %d)",
             command, argv, envp, infd, outfd, errfd);

 /*
  * Block signals before forking...
  */

#ifdef HAVE_SIGSET
  sighold(SIGTERM);
  sighold(SIGCHLD);
#elif defined(HAVE_SIGACTION)
  sigemptyset(&newmask);
  sigaddset(&newmask, SIGTERM);
  sigaddset(&newmask, SIGCHLD);
  sigprocmask(SIG_BLOCK, &newmask, &oldmask);
#endif /* HAVE_SIGSET */

  if ((*pid = fork()) == 0)
  {
   /*
    * Child process goes here...
    *
    * Reset signal handlers
    */

#ifdef HAVE_SIGSET /* Use System V signals over POSIX to avoid bugs */
    sigset(SIGCHLD, SIG_DFL);
    sigset(SIGTERM, SIG_DFL);

    sigrelse(SIGTERM);
    sigrelse(SIGCHLD);
#elif defined(HAVE_SIGACTION)
    memset(&action, 0, sizeof(action));
    
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGCHLD);
    action.sa_handler = SIG_DFL;
    sigaction(SIGCHLD, &action, NULL);
    
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGTERM);
    action.sa_handler = SIG_DFL;
    sigaction(SIGTERM, &action, NULL);

    sigprocmask(SIG_SETMASK, &oldmask, NULL);
#else
    signal(SIGCHLD, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
#endif /* HAVE_SIGSET */
    
    /*
    * Update stdin/stdout/stderr as needed...
    */

    close(0);
    dup(infd);
    close(1);
    dup(outfd);
    if (errfd > 2)
    {
      close(2);
      dup(errfd);
    }

   /*
    * Close extra file descriptors...
    */

    for (fd = 3; fd < MaxFDs; fd ++)
      close(fd);

   /*
    * Change user to something "safe"...
    */

    if (!root && getuid() == 0)
    {
     /*
      * Running as root, so change to non-priviledged user...
      */

      if (setgid(Group))
        exit(errno);

      if (setuid(User))
        exit(errno);
    }

   /*
    * Reset group membership to just the main one we belong to.
    */

    setgroups(0, NULL);

   /*
    * Change umask to restrict permissions on created files...
    */

    umask(077);

   /*
    * Execute the command; if for some reason this doesn't work,
    * return the error code...
    */

    execve(command, argv, envp);

    perror(command);

    exit(errno);
  }
  else if (*pid < 0)
  {
   /*
    * Error - couldn't fork a new process!
    */

    LogMessage(L_ERROR, "Unable to fork %s - %s.", command, strerror(errno));

    *pid = 0;
  }

#ifdef HAVE_SIGSET
  sigrelse(SIGTERM);
  sigrelse(SIGCHLD);
#elif defined(HAVE_SIGACTION)
  sigprocmask(SIG_SETMASK, &oldmask, NULL);
#endif /* HAVE_SIGSET */

  return (*pid);
}


/*
 * End of "$Id: job.c,v 1.5.2.2 2002/09/10 05:56:38 jlovell Exp $".
 */