#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ(2.59)
AC_INIT(UGLY, 0.01)
AC_CONFIG_SRCDIR([lib/c/ugly.h])
AM_CONFIG_HEADER([config.h])
AM_INIT_AUTOMAKE
AC_CONFIG_MACRO_DIR([m4])
LT_INIT

# Checks for programs.
AC_PROG_INSTALL
AC_LANG_C
AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_MAKE_SET

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
