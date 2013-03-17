#!/bin/bash

CLASSPATH="$1"
LD_LIBRARY_PATH="$2"

TESTS=t/t[0-9]*.java

for t in $TESTS ; do
	java -cp "$CLASSPATH" -Djna.library.path="$LD_LIBRARY_PATH" $(basename $t .java)
	# perl -e "use Test::Harness qw(&runtests \$verbose); \$verbose='1'; runtests @ARGV;" $TESTS
done
