#!/bin/bash

echo
export UGLY_TEST_LANG=${UGLY_TEST_LANG:-perl5}
export UGLY_TEST_OPTS=${UGLY_TEST_OPTS:-}
LD_LIBRARY_PATH=../lang/$UGLY_TEST_LANG/.libs:$LD_LIBRARY_PATH

if [ -f $UGLY_TEST_LANG/test.env ] ; then
	. $UGLY_TEST_LANG/test.env
else
	echo "Can't locate $UGLY_TEST_LANG/test.env" >&2
	exit 1
fi

export LD_LIBRARY_PATH
echo LD_LIBRARY_PATH="$LD_LIBRARY_PATH"
export HARNESS_PERL="./test"

TESTS=
if [ -n "$1" ] ; then
	echo "Using \$1 ($1) as test list"
	TESTS="$1"
elif [ -n "$UGLY_TEST_ONLY" ] ; then
	echo "Using \$UGLY_TEST_ONLY ($UGLY_TEST_ONLY) as test list"
	TESTS="$UGLY_TEST_ONLY"
else
	TESTS=t/*.t
fi

if [ -n "UGLY_TEST_NO_HARNESS" ] ; then
	for t in $TESTS ; do
		./test $t
	done
else
	export UGLY_TEST_UNDER_HARNESS=1
	perl -e "use Test::Harness qw(&runtests \$verbose); \$verbose='1'; runtests @ARGV;" $TESTS
fi
