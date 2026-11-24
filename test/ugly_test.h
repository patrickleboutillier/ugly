/*
 * ugly_test.h: a minimal, dependency-free TAP producer for UGLY's C tests.
 *
 * Each test program is a single TAP stream. The plan ("1..N") is printed at
 * the end by done_testing(), so tests never have to count themselves. The
 * output is consumed by automake's tap-driver.sh (make check), and also works
 * with prove(1) or meson's 'tap' protocol.
 *
 *   int main(void){
 *       ugly_context *ctx = ugly_context_new() ;
 *       ugly_language *lang = ugly_language_init(ctx, ugly_test_lang()) ;
 *       if (! no_error(ctx, "language %s initialized", ugly_test_lang()))
 *           return done_testing() ;
 *       is_string(ugly_language_get_name(lang), ugly_test_lang(), "name") ;
 *       return done_testing() ;
 *   }
 *
 * The plugin under test is chosen with UGLY_TEST_LANG (default: perl5).
 */
#ifndef _UGLY_TEST_H_
#define _UGLY_TEST_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>

#include "ugly.h"


static int _ut_count = 0 ;
static int _ut_failed = 0 ;


static void _ut_line(int pass, const char *directive, const char *reason, const char *fmt, va_list ap){
	printf("%s %d", (pass ? "ok" : "not ok"), ++_ut_count) ;
	if ((fmt != NULL)&&(fmt[0] != '\0')){
		printf(" - ") ;
		vprintf(fmt, ap) ;
	}
	if (directive != NULL){
		printf(" # %s", directive) ;
		if ((reason != NULL)&&(reason[0] != '\0')){
			printf(" %s", reason) ;
		}
	}
	printf("\n") ;
	if (! pass){
		_ut_failed++ ;
	}
	fflush(stdout) ;
}


/* Print a diagnostic line (a TAP comment). */
static void diag(const char *fmt, ...){
	va_list ap ;
	va_start(ap, fmt) ;
	printf("# ") ;
	vprintf(fmt, ap) ;
	printf("\n") ;
	fflush(stdout) ;
	va_end(ap) ;
}


static int _ut_ok(const char *file, int line, int pass, const char *fmt, ...){
	va_list ap ;
	va_start(ap, fmt) ;
	_ut_line(pass, NULL, NULL, fmt, ap) ;
	va_end(ap) ;
	if (! pass){
		diag("  Failed test at %s line %d.", file, line) ;
	}
	return pass ;
}


static int _ut_is_long(const char *file, int line, long got, long expected, const char *fmt, ...){
	int pass = (got == expected) ;
	va_list ap ;
	va_start(ap, fmt) ;
	_ut_line(pass, NULL, NULL, fmt, ap) ;
	va_end(ap) ;
	if (! pass){
		diag("  Failed test at %s line %d.", file, line) ;
		diag("         got: %ld", got) ;
		diag("    expected: %ld", expected) ;
	}
	return pass ;
}


static int _ut_is_double(const char *file, int line, double got, double expected, const char *fmt, ...){
	int pass = (fabs(got - expected) <= 1e-9 * fmax(1.0, fabs(expected))) ;
	va_list ap ;
	va_start(ap, fmt) ;
	_ut_line(pass, NULL, NULL, fmt, ap) ;
	va_end(ap) ;
	if (! pass){
		diag("  Failed test at %s line %d.", file, line) ;
		diag("         got: %.17g", got) ;
		diag("    expected: %.17g", expected) ;
	}
	return pass ;
}


static int _ut_is_string(const char *file, int line, const char *got, const char *expected, const char *fmt, ...){
	int pass = (got == NULL || expected == NULL) ? (got == expected) : (strcmp(got, expected) == 0) ;
	va_list ap ;
	va_start(ap, fmt) ;
	_ut_line(pass, NULL, NULL, fmt, ap) ;
	va_end(ap) ;
	if (! pass){
		diag("  Failed test at %s line %d.", file, line) ;
		diag("         got: %s%s%s", got ? "'" : "", got ? got : "NULL", got ? "'" : "") ;
		diag("    expected: %s%s%s", expected ? "'" : "", expected ? expected : "NULL", expected ? "'" : "") ;
	}
	return pass ;
}


#define ok(cond, ...)					_ut_ok(__FILE__, __LINE__, ((cond) ? 1 : 0), __VA_ARGS__)
#define is_long(got, expected, ...)		_ut_is_long(__FILE__, __LINE__, (got), (expected), __VA_ARGS__)
#define is_double(got, expected, ...)	_ut_is_double(__FILE__, __LINE__, (got), (expected), __VA_ARGS__)
#define is_string(got, expected, ...)	_ut_is_string(__FILE__, __LINE__, (got), (expected), __VA_ARGS__)


/* Record one skipped test. */
static void skip(const char *reason, const char *fmt, ...){
	va_list ap ;
	va_start(ap, fmt) ;
	_ut_line(1, "SKIP", reason, fmt, ap) ;
	va_end(ap) ;
}


static int _ut_no_error(const char *file, int line, const ugly_context *ctx, const char *fmt, ...){
	va_list ap ;
	va_start(ap, fmt) ;
	ugly_error err = ugly_context_get_error(ctx) ;
	if (err == UGLY_OK){
		_ut_line(1, NULL, NULL, fmt, ap) ;
		va_end(ap) ;
		return 1 ;
	}
	if ((err == UGLY_NOT_SUPPORTED)||(err == UGLY_NOT_IMPLEMENTED)){
		_ut_line(1, "SKIP", ugly_context_get_error_msg(ctx), fmt, ap) ;
		va_end(ap) ;
		return 0 ;
	}
	_ut_line(0, NULL, NULL, fmt, ap) ;
	va_end(ap) ;
	diag("  Failed test at %s line %d.", file, line) ;
	diag("  UGLY error %d: %s", err, ugly_context_get_error_msg(ctx) ? ugly_context_get_error_msg(ctx) : "(no message)") ;
	return 0 ;
}


static int _ut_has_error(const char *file, int line, const ugly_context *ctx, ugly_error expected, const char *fmt, ...){
	va_list ap ;
	va_start(ap, fmt) ;
	ugly_error err = ugly_context_get_error(ctx) ;
	int pass = (err == expected) ;
	_ut_line(pass, NULL, NULL, fmt, ap) ;
	va_end(ap) ;
	if (! pass){
		diag("  Failed test at %s line %d.", file, line) ;
		diag("         got: UGLY error %d (%s)", err, ugly_context_get_error_msg(ctx) ? ugly_context_get_error_msg(ctx) : "no message") ;
		diag("    expected: UGLY error %d", expected) ;
	}
	return pass ;
}


/*
 * no_error(ctx, ...): passes if the last UGLY call succeeded, and returns 1.
 * If the plugin reported UGLY_NOT_SUPPORTED or UGLY_NOT_IMPLEMENTED, records
 * a skip and returns 0. Any other error fails and returns 0. Use the return
 * value to guard the checks that depend on the call:
 *
 *     ugly_value *v = ugly_runtime_call_function(...) ;
 *     if (no_error(ctx, "add(1, 2) called")){ ... }
 */
#define no_error(ctx, ...)				_ut_no_error(__FILE__, __LINE__, (ctx), __VA_ARGS__)

/* has_error(ctx, code, ...): passes if the last UGLY call set error 'code'. */
#define has_error(ctx, code, ...)		_ut_has_error(__FILE__, __LINE__, (ctx), (code), __VA_ARGS__)


/* The language plugin under test. */
static const char *ugly_test_lang(void){
	const char *l = getenv("UGLY_TEST_LANG") ;
	return ((l != NULL)&&(l[0] != '\0')) ? l : "perl5" ;
}


/* Print the plan and return the process exit status. */
static int done_testing(void){
	printf("1..%d\n", _ut_count) ;
	fflush(stdout) ;
	return (_ut_failed > 0 ? 1 : 0) ;
}


#endif
