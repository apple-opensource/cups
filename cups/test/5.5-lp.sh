#!/bin/sh
#
# "$Id: 5.5-lp.sh 6649 2007-07-11 21:46:42Z mike $"
#
#   Test the lp command.
#
#   Copyright 2007 by Apple Inc.
#   Copyright 1997-2005 by Easy Software Products, all rights reserved.
#
#   These coded instructions, statements, and computer programs are the
#   property of Apple Inc. and are protected by Federal copyright
#   law.  Distribution and use rights are outlined in the file "LICENSE.txt"
#   which should have been included with this file.  If this file is
#   file is missing or damaged, see the license at "http://www.cups.org/".
#

echo "LP Default Test"
echo ""
echo "    lp testfile.jpg"
../systemv/lp testfile.jpg 2>&1
if test $? != 0; then
	echo "    FAILED"
	exit 1
else
	echo "    PASSED"
fi
echo ""

echo "LP Destination Test"
echo ""
echo "    lp -d Test1 testfile.jpg"
../systemv/lp -d Test1 -o job-hold-until=indefinite testfile.jpg 2>&1
if test $? != 0; then
	echo "    FAILED"
	exit 1
else
	echo "    PASSED"
fi
echo ""

echo "LP Flood Test"
echo ""
echo "    lp -d Test1 testfile.jpg ($1 times in parallel)"
i=0
while test $i -lt $1; do
	echo "    flood copy $i..." 1>&2
	../systemv/lp -d Test1 testfile.jpg 2>&1 &
	lppid=$!
	i=`expr $i + 1`
done
wait $lppid
if test $? != 0; then
	echo "    FAILED"
	exit 1
else
	echo "    PASSED"
fi
echo ""

#
# End of "$Id: 5.5-lp.sh 6649 2007-07-11 21:46:42Z mike $".
#
