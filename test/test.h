#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/resource.h>


#include <tap.h>
#include "../lib/c/ugly.h"


#define DELTA 1e-10
#define EQUALS(A, B) (((A-B) < DELTA)&&((A-B) > -DELTA))


int do_test ;


#define UGLY_CHECK_ERROR(ctx, n) \
	do_test = 1 ; \
	if (ugly_context_has_error(ctx)){ \
		printf("\n") ; \
		switch (ugly_context_get_error(ctx)){ \
			case UGLY_NOT_SUPPORTED: \
				skip(n, "Not supported by this language: %s", ugly_context_get_error_msg(ctx)) ; \
				break ; \
			case UGLY_NOT_IMPLEMENTED: \
				skip(n, "Not implemented yet in this language: %s", ugly_context_get_error_msg(ctx)) ; \
				break ; \
			default: { \
				int i ; \
				for (i = 0 ; i < n ; i++){ \
					fail("Code block skipped (test %d of %d) caused by previous UGLY error (%d: %s)", i+1, n, \
						ugly_context_get_error(ctx), ugly_context_get_error_msg(ctx)) ; \
				} \
				break ; \
			} \
		} \
		do_test = 0 ; \
	} \
	if (do_test)

