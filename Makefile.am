SUBDIRS = lib
SUBDIRS += lang
SUBDIRS += test

ACLOCAL_AMFLAGS = -I m4

# The C tests again, under valgrind. See test/Makefile.am.
check-valgrind: all
	cd test && $(MAKE) $(AM_MAKEFLAGS) check-valgrind

.PHONY: check-valgrind
