#!/bin/sh
#
# "$Id: cups.sh,v 1.20 2002/06/10 23:47:23 jlovell Exp $"
#
#   Startup/shutdown script for the Common UNIX Printing System (CUPS).
#
#   Copyright 1997-2002 by Easy Software Products, all rights reserved.
#
#   These coded instructions, statements, and computer programs are the
#   property of Easy Software Products and are protected by Federal
#   copyright law.  Distribution and use rights are outlined in the file
#   "LICENSE.txt" which should have been included with this file.  If this
#   file is missing or damaged please contact Easy Software Products
#   at:
#
#       Attn: CUPS Licensing Information
#       Easy Software Products
#       44141 Airport View Drive, Suite 204
#       Hollywood, Maryland 20636-3111 USA
#
#       Voice: (301) 373-9603
#       EMail: cups-info@cups.org
#         WWW: http://www.cups.org
#

#### OS-Dependent Information

#
#   Linux chkconfig stuff:
#
#   chkconfig: 235 99 00
#   description: Startup/shutdown script for the Common UNIX \
#                Printing System (CUPS).
#

#
#   NetBSD 1.5+ rcorder script lines.  The format of the following two
#   lines is very strict -- please don't add additional spaces!
#
# PROVIDE: cups
# REQUIRE: DAEMON
#


#### OS-Dependent Configuration

case "`uname`" in
	IRIX*)
		IS_ON=/sbin/chkconfig
		;;

	*BSD*)
        	IS_ON=:
		;;

	Darwin*)
		. /etc/rc.common

		if test "${CUPS:=-YES-}" = "-NO-"; then
			exit 0
		fi

        	IS_ON=:
		;;

	Linux*)
		# Set the timezone, if possible...
		if test -f /etc/TIMEZONE; then
                        . /etc/TIMEZONE
                else
			if test -f /etc/sysconfig/clock; then
	                        . /etc/sysconfig/clock
        	                TZ="$ZONE"
                	        export TZ
			fi
                fi

		IS_ON=/bin/true
		;;

	*)
		IS_ON=/bin/true
		;;
esac

#### OS-Independent Stuff

#
# The verbose flag controls the printing of the names of
# daemons as they are started.  Currently always echos for
# all but IRIX, which can configure verbose bootup messages.
#

if test "`uname`" = "Darwin"; then
	ECHO=ConsoleMessage
else
	if $IS_ON verbose; then
		ECHO=echo
	else
		ECHO=:
	fi
fi

#
# See if the CUPS server (cupsd) is running...
#

case "`uname`" in
	HP-UX* | AIX* | SINIX*)
		pid=`ps -e | awk '{if (match($4, ".*/cupsd$") || $4 == "cupsd") print $1}'`
		;;
	IRIX* | SunOS*)
		pid=`ps -e | nawk '{if (match($4, ".*/cupsd$") || $4 == "cupsd") print $1}'`
		;;
	UnixWare*)
		pid=`ps -e | awk '{if (match($6, ".*/cupsd$") || $6 == "cupsd") print $1}'`
		. /etc/TIMEZONE
		;;
	OSF1*)
		pid=`ps -e | awk '{if (match($5, ".*/cupsd$") || $5 == "cupsd") print $1}'`
		;;
	Linux* | *BSD* | Darwin*)
		pid=`ps ax | awk '{if (match($5, ".*/cupsd$") || $5 == "cupsd") print $1}'`
		;;
	*)
		pid=""
		;;
esac

#
# Start or stop the CUPS server based upon the first argument to the script.
#

case $1 in
	start | restart | reload)
		if $IS_ON cups; then
			if test "$pid" != ""; then
				kill -HUP $pid
			else
				prefix=/
				exec_prefix=/usr
				${exec_prefix}/sbin/cupsd
			fi
			$ECHO "cups: scheduler ${1}ed."
		else
			$ECHO "cups: scheduler stopped."
		fi
		;;

	stop)
		if test "$pid" != ""; then
			kill $pid
			$ECHO "cups: scheduler stopped."
		fi
		;;

	status)
		if test "$pid" != ""; then
			echo "cups: Scheduler is running."
		else
			echo "cups: Scheduler is not running."
		fi
		;;

	*)
		echo "Usage: cups {reload|restart|start|status|stop}"
		exit 1
		;;
esac

#
# Exit with no errors.
#

exit 0


#
# End of "$Id: cups.sh,v 1.20 2002/06/10 23:47:23 jlovell Exp $".
#