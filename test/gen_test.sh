#!/bin/sh


TESTS="t/*.t"

echo '#include "test.h"'
for t in $TESTS ; do
	name=$(basename $t .t)
	echo
	echo "void t_$name(const char *UGLY_TEST_LANG, const char *UGLY_TEST_OPTS){"
	echo "#line 1 \"$t\""
	cat $t | (while read line ; do 
		echo -e "\t$line"
	done)
	echo }
done

echo
echo 'int main(int argc, char *argv[]){'
echo '	const char *l = getenv("UGLY_TEST_LANG") ;'
echo '	const char *o = getenv("UGLY_TEST_OPTS") ;'
echo '	if (argc < 2) { fprintf(stderr, "Usage: test FILE\n") ; exit(1) ; }'
for t in $TESTS ; do
	name=$(basename $t .t)
	echo "	if (strcmp(argv[argc-1], \"$t\") == 0) { t_$name(l, o) ; }"
done
echo '	return exit_status() ;'
echo "}"


