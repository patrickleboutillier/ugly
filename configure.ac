#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT(UGLY, 0.01)
AC_CONFIG_SRCDIR([lib/c/ugly.h])
AM_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE([subdir-objects])
AC_REQUIRE_AUX_FILE([tap-driver.sh])
AC_CONFIG_MACRO_DIR([m4])
LT_INIT

# Checks for programs.
AC_PROG_INSTALL
AC_LANG_C
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_MAKE_SET
AC_PROG_AWK
AC_PATH_PROG([PERL], [perl])
AS_IF([test -z "$PERL"], [AC_MSG_ERROR([perl is required])])
AC_MSG_CHECKING([for ExtUtils::Embed])
AS_IF([$PERL -MExtUtils::Embed -e 1 2>/dev/null], [AC_MSG_RESULT([yes])], [AC_MSG_ERROR([ExtUtils::Embed is required to build the perl5 plugin])])
# Optional: only needed for 'make check-valgrind'.
AC_PATH_PROG([VALGRIND], [valgrind])

# Checks for libraries.
AC_CHECK_LIB([dl], [dlopen])

# Checks for header files.
AC_HEADER_STDC
AC_CHECK_HEADERS([stdio.h])

# Checks for typedefs, structures, and compiler characteristics.
AC_C_CONST

# Checks for library functions.
AC_FUNC_MALLOC
AC_CHECK_FUNCS([strdup])

AC_CONFIG_FILES([
	Makefile 
	lib/Makefile
	lib/c/Makefile
	lang/Makefile
	lang/perl5/Makefile
	test/Makefile
])


AC_OUTPUT
