/*
 * Starting GHC's runtime system, compiled by ghc into libugly_rts.so, with
 * the RTS linked: ghc knows where its own RTS headers are (they moved in
 * GHC 9.4), so ugly_haskell.so, built by Meson, needs none of them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <HsFFI.h>
#include <Rts.h>

/*
 * Once per process: the RTS can't be stopped and started again (hs_exit,
 * then hs_init, isn't supported), so it's never stopped. Its signal handlers
 * (SIGINT raising UserInterrupt, SIGPIPE ignored...) stay off: signals are
 * the host's. 'opts' are more RTS options, as between +RTS and -RTS (-N4,
 * -M1g), or "". Returns 0, or -1 if out of memory.
 */
int ugly_hs_start(const char *opts){
	static const char fixed[] = "--install-signal-handlers=no" ;
	static char *args[] = { "ugly", NULL } ;
	int argc = 1 ;
	char **argv = args ;
	size_t size = sizeof fixed + 1 + strlen(opts) ;
	char *all = malloc(size) ;     /* kept: the RTS is never stopped */
	if (all == NULL){
		return -1 ;
	}
	snprintf(all, size, "%s %s", fixed, opts) ;

	RtsConfig conf = defaultRtsConfig ;
	conf.rts_opts_enabled = RtsOptsAll ;
	/* Module libraries are dlopen'ed after the RTS starts, and nothing tells
	 * it their foreign exports are roots (GHC 9's are processed only by
	 * hs_init and its own linker): without this, a CAF such as a module's
	 * box could be collected while its code can still enter it. */
	conf.keep_cafs = HS_BOOL_TRUE ;
	conf.rts_opts = all ;
	hs_init_ghc(&argc, &argv, conf) ;
	return 0 ;
}
