/*
 * "$Id: ipp-var.c,v 1.2 2003/07/23 04:05:06 jlovell Exp $"
 *
 *   IPP variable routines for the Common UNIX Printing System (CUPS).
 *
 *   Copyright 1997-2003 by Easy Software Products.
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
 *   ippGetAttributes()    - Get the list of attributes that are needed
 *                           by the template file.
 *   ippGetTemplateDir()   - Get the templates directory...
 *   ippSetServerVersion() - Set the server name and CUPS version...
 *   ippSetCGIVars()       - Set CGI variables from an IPP response.
 */

/*
 * Include necessary headers...
 */

#include "ipp-var.h"


/*
 * 'ippGetAttributes()' - Get the list of attributes that are needed
 *                        by the template file.
 */

void
ippGetAttributes(ipp_t      *request,	/* I - IPP request */
                 const char *directory,	/* I - Directory */
		 const char *tmpl,	/* I - Base filename */
		 const char *lang)	/* I - Language */
{
  int	num_attrs;			/* Number of attributes */
  char	*attrs[1000];			/* Attributes */
  int	i;				/* Looping var */
  char	filename[1024],			/* Filename */
	locale[16];			/* Locale name */
  FILE	*in;				/* Input file */
  int	ch;				/* Character from file */
  char	name[255],			/* Name of variable */
	*nameptr;			/* Pointer into name */


 /*
  * Convert the language to a locale name...
  */

  if (lang != NULL)
  {
    for (i = 0; lang[i] && i < 15; i ++)
      if (isalnum(lang[i]))
        locale[i] = tolower(lang[i]);
      else
        locale[i] = '_';

    locale[i] = '\0';
  }
  else
    locale[0] = '\0';

 /*
  * See if we have a template file for this language...
  */

  snprintf(filename, sizeof(filename), "%s/%s/%s", directory, locale, tmpl);
  if (access(filename, 0))
  {
    locale[2] = '\0';

    snprintf(filename, sizeof(filename), "%s/%s/%s", directory, locale, tmpl);
    if (access(filename, 0))
      snprintf(filename, sizeof(filename), "%s/%s", directory, tmpl);
  }

 /*
  * Open the template file...
  */

  if ((in = fopen(filename, "r")) == NULL)
    return;

 /*
  * Loop through the file adding attribute names as needed...
  */

  num_attrs = 0;

  while ((ch = getc(in)) != EOF)
    if (ch == '\\')
      getc(in);
    else if (ch == '{' && num_attrs < (sizeof(attrs) / sizeof(attrs[0])))
    {
     /*
      * Grab the name...
      */

      for (nameptr = name; (ch = getc(in)) != EOF;)
        if (strchr("}]<>=! \t\n", ch))
          break;
        else if (nameptr > name && ch == '?')
	  break;
	else if (nameptr < (name + sizeof(name) - 1))
	{
	  if (ch == '_')
	    *nameptr++ = '-';
	  else
            *nameptr++ = ch;
	}

      *nameptr = '\0';

     /*
      * Possibly add it to the list of attributes...
      */

      for (i = 0; i < num_attrs; i ++)
        if (!strcmp(attrs[i], name))
	  break;

      if (i >= num_attrs)
      {
	attrs[num_attrs] = strdup(name);
	num_attrs ++;
      }
    }

 /*
  * If we have attributes, add a requested-attributes attribute to the
  * request...
  */

  if (num_attrs > 0)
  {
    ippAddStrings(request, IPP_TAG_OPERATION, IPP_TAG_KEYWORD,
                  "requested-attributes", num_attrs, NULL, (const char **)attrs);

    for (i = 0; i < num_attrs; i ++)
      free(attrs[i]);
  }
}


/*
 * 'ippGetTemplateDir()' - Get the templates directory...
 */

char *					/* O - Template directory */
ippGetTemplateDir(void)
{
  const char	*datadir;		/* CUPS_DATADIR env var */
  static char	templates[1024] = "";	/* Template directory */


  if (!templates[0])
  {
   /*
    * Build the template directory pathname...
    */

    if ((datadir = getenv("CUPS_DATADIR")) == NULL)
      datadir = CUPS_DATADIR;

    snprintf(templates, sizeof(templates), "%s/templates", datadir);
  }

  return (templates);
}


/*
 * 'ippSetServerVersion()' - Set the server name and CUPS version...
 */

void
ippSetServerVersion(void)
{
  cgiSetVariable("SERVER_NAME", getenv("SERVER_NAME"));
  cgiSetVariable("REMOTE_USER", getenv("REMOTE_USER"));
  cgiSetVariable("CUPS_VERSION", CUPS_SVERSION);

#ifdef LC_TIME
  setlocale(LC_TIME, "");
#endif /* LC_TIME */
}


/*
 * 'ippSetCGIVars()' - Set CGI variables from an IPP response.
 */

void
ippSetCGIVars(ipp_t      *response,	/* I - Response data to be copied... */
              const char *filter_name,	/* I - Filter name */
	      const char *filter_value,	/* I - Filter value */
	      const char *prefix,	/* I - Prefix for name or NULL */
	      int        parent_el)	/* I - Parent element number */
{
  int			element;	/* Element in CGI array */
  ipp_attribute_t	*attr,		/* Attribute in response... */
			*filter;	/* Filtering attribute */
  int			i;		/* Looping var */
  char			name[1024],	/* Name of attribute */
			*nameptr,	/* Pointer into name */
			value[16384],	/* Value(s) */
			*valptr;	/* Pointer into value */
  char			method[HTTP_MAX_URI],
			username[HTTP_MAX_URI],
			hostname[HTTP_MAX_URI],
			rawresource[HTTP_MAX_URI],
			resource[HTTP_MAX_URI],
			uri[HTTP_MAX_URI],
			*rawptr,	/* Pointer into rawresource */
			*resptr;	/* Pointer into resource */
  int			port;		/* URI data */
  int			ishttps;	/* Using encryption? */
  const char		*server;	/* Name of server */
  char			servername[1024];
					/* Locale server name */
  struct tm		*date;		/* Date information */
  static const char	hexchars[] = "0123456789ABCDEF";
					/* Hexadecimal conversion characters */


  DEBUG_printf(("<P>ippSetCGIVars(response=%p, filter_name=\"%s\", filter_value=\"%s\", prefix=\"%s\")\n",
                response, filter_name, filter_value, prefix));
  
 /*
  * Set common CGI template variables...
  */

  if (!prefix)
    ippSetServerVersion();

 /*
  * Get the server name associated with the client interface as well as
  * the locally configured hostname.  We'll check *both* of these to
  * see if the printer URL is local...
  */

  server = getenv("SERVER_NAME");
  gethostname(servername, sizeof(servername));

 /*
  * Flag whether we are using SSL on this connection...
  */

  ishttps = getenv("HTTPS") != NULL;

 /*
  * Loop through the attributes and set them for the template...
  */

  attr = response->attrs;

  if (!prefix)
    while (attr && attr->group_tag == IPP_TAG_OPERATION)
     attr = attr->next;

  for (element = parent_el; attr != NULL; attr = attr->next, element ++)
  {
   /*
    * Copy attributes to a separator...
    */

    if (filter_name)
    {
      for (filter = attr;
           filter != NULL && filter->group_tag != IPP_TAG_ZERO;
           filter = filter->next)
        if (filter->name && strcmp(filter->name, filter_name) == 0 &&
	    (filter->value_tag == IPP_TAG_STRING ||
	     (filter->value_tag >= IPP_TAG_TEXTLANG &&
	      filter->value_tag <= IPP_TAG_MIMETYPE)) &&
	    filter->values[0].string.text != NULL &&
	    strcasecmp(filter->values[0].string.text, filter_value) == 0)
	  break;

      if (!filter)
        return;

      if (filter->group_tag == IPP_TAG_ZERO)
      {
        attr = filter;
	element --;
	continue;
      }
    }

    for (; attr != NULL && attr->group_tag != IPP_TAG_ZERO; attr = attr->next)
    {
     /*
      * Copy the attribute name, substituting "_" for "-"...
      */

      if (attr->name == NULL)
        continue;

      if (prefix)
      {
        snprintf(name, sizeof(name), "%s.", prefix);
	nameptr = name + strlen(name);
      }
      else
        nameptr = name;

      for (i = 0; attr->name[i] && nameptr < (name + sizeof(name) - 1); i ++)
        if (attr->name[i] == '-')
	  *nameptr++ = '_';
	else
          *nameptr++ = attr->name[i];

      *nameptr = '\0';

     /*
      * Add "job_printer_name" variable if we have a "job_printer_uri"
      * attribute...
      */

      if (strcmp(name, "job_printer_uri") == 0)
      {
        if ((valptr = strrchr(attr->values[0].string.text, '/')) == NULL)
	  valptr = "unknown";
	else
	  valptr ++;

        cgiSetArray("job_printer_name", element, valptr);
      }

     /*
      * Copy values...
      */

      value[0] = '\0';	/* Initially an empty string */
      valptr   = value; /* Start at the beginning */

      for (i = 0; i < attr->num_values; i ++)
      {
	if (i)
	  strlcat(valptr, ",", sizeof(value) - (valptr - value));

	valptr += strlen(valptr);

	switch (attr->value_tag)
	{
	  case IPP_TAG_INTEGER :
	  case IPP_TAG_ENUM :
	      if (strncmp(name, "time_at_", 8) == 0)
	      {
	        date = localtime((time_t *)&(attr->values[i].integer));
		strftime(valptr, sizeof(value) - (valptr - value),
		         CUPS_STRFTIME_FORMAT, date);
	      }
	      else
	        snprintf(valptr, sizeof(value) - (valptr - value),
		         "%d", attr->values[i].integer);
	      break;

	  case IPP_TAG_BOOLEAN :
	      snprintf(valptr, sizeof(value) - (valptr - value),
	               "%d", attr->values[i].boolean);
	      break;

	  case IPP_TAG_NOVALUE :
	      strlcat(valptr, "novalue", sizeof(value) - (valptr - value));
	      break;

	  case IPP_TAG_RANGE :
	      snprintf(valptr, sizeof(value) - (valptr - value),
	               "%d-%d", attr->values[i].range.lower,
		       attr->values[i].range.upper);
	      break;

	  case IPP_TAG_RESOLUTION :
	      snprintf(valptr, sizeof(value) - (valptr - value),
	               "%dx%d%s", attr->values[i].resolution.xres,
		       attr->values[i].resolution.yres,
		       attr->values[i].resolution.units == IPP_RES_PER_INCH ?
			   "dpi" : "dpc");
	      break;

	  case IPP_TAG_URI :
	      if (strchr(attr->values[i].string.text, ':') != NULL)
	      {
	       /*
	        * Rewrite URIs...
		*/

		httpSeparate(attr->values[i].string.text, method, username,
		             hostname, &port, rawresource);

        	if (strcmp(method, "ipp") == 0 ||
	            strcmp(method, "http") == 0)
        	{
        	 /*
	          * Rewrite the resource string so it doesn't contain any
		  * illegal chars...
		  */

                  for (rawptr = rawresource, resptr = resource; *rawptr;)
		    if (*rawptr & 128 || *rawptr == '%' || *rawptr == ' ')
		    {
		      if (resptr < (resource + sizeof(resource) - 3))
		      {
		        *resptr++ = '%';
			*resptr++ = hexchars[(*rawptr >> 4) & 15];
			*resptr++ = hexchars[*rawptr & 15];
		      }
		    }
		    else if (resptr < (resource + sizeof(resource) - 1))
		      *resptr++ = *rawptr++;

                  *resptr = '\0';

        	 /*
		  * Map local access to a local URI...
		  */

        	  if (strcasecmp(hostname, server) == 0 ||
		      strcasecmp(hostname, servername) == 0)
		  {
		   /*
		    * Make URI relative to the current server...
		    */

                    strlcpy(uri, resource, sizeof(uri));
		  }
		  else
		  {
        	   /*
		    * Rewrite URI with HTTP address...
		    */

		    if (username[0])
		      snprintf(uri, sizeof(uri), "%s://%s@%s:%d%s",
		               ishttps ? "https" : "http",
		               username, hostname, port, resource);
        	    else
		      snprintf(uri, sizeof(uri), "%s://%s:%d%s", 
		               ishttps ? "https" : "http",
			       hostname, port, resource);
                  }

		  strlcat(valptr, uri, sizeof(value) - (valptr - value));
        	  break;
        	}
              }

          case IPP_TAG_STRING :
	  case IPP_TAG_TEXT :
	  case IPP_TAG_NAME :
	  case IPP_TAG_KEYWORD :
	  case IPP_TAG_CHARSET :
	  case IPP_TAG_LANGUAGE :
	  case IPP_TAG_MIMETYPE :
	      strlcat(valptr, attr->values[i].string.text,
	              sizeof(value) - (valptr - value));
	      break;

          case IPP_TAG_BEGIN_COLLECTION :
	      snprintf(value, sizeof(value), "%s%d", name, i + 1);
              ippSetCGIVars(attr->values[i].collection, filter_name,
	                    filter_value, value, element);
              break;

          default :
	      break; /* anti-compiler-warning-code */
	}
      }

     /*
      * Add the element...
      */

      if (attr->value_tag != IPP_TAG_BEGIN_COLLECTION)
      {
        cgiSetArray(name, element, value);

        DEBUG_printf(("<P>%s[%d]=\"%s\"\n", name, element, value));
      }
    }

    if (attr == NULL)
      break;
  }

  DEBUG_puts("<P>Leaving ippSetCGIVars()...");
}


/*
 * End of "$Id: ipp-var.c,v 1.2 2003/07/23 04:05:06 jlovell Exp $".
 */