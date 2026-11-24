# UGLY test suite.
#
#   make check                          # run everything against the perl5 plugin
#   make check UGLY_TEST_LANG=<lang>    # run the same suite against another plugin
#   make check TESTS=t/object           # run a single test
#
# Each test is a TAP stream, run through automake's tap-driver.sh; see
# test/<name>.log for the full output. UGLY_DEBUG=<n> adds trace output.

AM_CPPFLAGS = -I$(top_srcdir)/lib/c -I$(srcdir)
LDADD = $(top_builddir)/lib/c/libugly.la -lm

# The C API, run against the plugin selected by UGLY_TEST_LANG.
C_TESTS = t/context t/value t/language t/runtime t/object
check_PROGRAMS = $(C_TESTS)
noinst_HEADERS = ugly_test.h

# The Perl binding (lib/perl5, built by MakeMaker), also driving that plugin.
PERL_BINDING_TESTS = \
	../lib/perl5/t/01_context.t \
	../lib/perl5/t/02_value.t \
	../lib/perl5/t/03_language.t \
	../lib/perl5/t/04_runtime.t

TESTS = $(C_TESTS) $(PERL_BINDING_TESTS)

TEST_EXTENSIONS = .t
LOG_DRIVER = env AM_TAP_AWK='$(AWK)' $(SHELL) $(top_srcdir)/tap-driver.sh
T_LOG_DRIVER = $(LOG_DRIVER)
T_LOG_COMPILER = $(PERL)

# Plugins are found by dlopen() through LD_LIBRARY_PATH, and their helper code
# under UGLY_LANGUAGE_DIR/<lang>/. Test libraries (calc, oocalc) live in
# test/<lang>/. PERL5LIB serves both the embedded perl and the binding tests.
AM_TESTS_ENVIRONMENT = \
	UGLY_TEST_LANG=$${UGLY_TEST_LANG:-perl5}; export UGLY_TEST_LANG; \
	UGLY_LANGUAGE_DIR='$(abs_top_srcdir)/lang'; export UGLY_LANGUAGE_DIR; \
	LD_LIBRARY_PATH="$(abs_top_builddir)/lang/$$UGLY_TEST_LANG/.libs:$(abs_top_builddir)/lib/c/.libs$${LD_LIBRARY_PATH:+:$$LD_LIBRARY_PATH}"; \
	export LD_LIBRARY_PATH; \
	PERL5LIB="$(abs_srcdir)/perl5:$(abs_top_builddir)/lib/perl5/blib/lib:$(abs_top_builddir)/lib/perl5/blib/arch$${PERL5LIB:+:$$PERL5LIB}"; \
	export PERL5LIB;

EXTRA_DIST = perl5/calc.pm perl5/oocalc.pm $(PERL_BINDING_TESTS)
