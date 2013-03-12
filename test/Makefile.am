# SUBDIRS = c

BUILT_SOURCES = 	test.c
SOURCES = 			test.c
test_SOURCES = 		test.c

TESTS =				test.sh

EXTRA_DIST = 		$(TESTS) gentest.sh

check_PROGRAMS = 	test

test_CFLAGS = 		-g -I../lib/c
test_LDFLAGS = 		-L/usr/local/lib -L../lib/c
test_LDADD = 		-ltap -lugly

CLEANFILES =		test.c *.log

test.c: gen_test.sh test.h t/*.t
	./gen_test.sh > test.c
