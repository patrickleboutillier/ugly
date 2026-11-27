/*
 * UGLY_TRAIT_FIXED_SIZE (contract §2, "A sequence may be unbounded"): the
 * core refuses ugly_splice on a sequence whose describe reports it, before
 * asking for its length, which may not return (a lazy Haskell list). A test
 * plugin, registered here, counts the calls the core makes into it.
 */
#include <stdio.h>
#include <string.h>

#include "ugly_plugin.h"

static int n_tests = 0, n_failed = 0 ;

static void ok(int good, const char *what){
	n_tests++ ;
	if (! good){
		n_failed++ ;
	}
	printf("%s %d - %s\n", good ? "ok" : "not ok", n_tests, what) ;
}


/* The test language: two sequences of 3 elements, one of fixed size. */
typedef struct seq {
	int fixed ;
	int lengths ;      /* calls to length */
	int splices ;      /* calls to splice */
} seq ;

static seq fixed_seq = { 1, 0, 0 }, open_seq = { 0, 0, 0 } ;
static int rt_token ;


static ugly_status t_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	(void)ctx ; (void)rt ;
	*native = &rt_token ;
	return UGLY_OK ;
}

static ugly_status t_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ; (void)native ;
	return UGLY_OK ;
}

static ugly_status t_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	seq *s = (strcmp(name, "fixed") == 0 ? &fixed_seq : &open_seq) ;
	ugly_handle h = UGLY_NO_HANDLE ;
	ugly_status st = ugly_export(ctx, rt, s, (uintptr_t)s, &h) ;
	if (st == UGLY_OK){
		*out = ugly_hval(h) ;
	}
	return st ;
}

static ugly_status t_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	(void)rt ;
	seq *s = ref ;
	*traits = UGLY_TRAIT_ELEMENTS | (s->fixed ? UGLY_TRAIT_FIXED_SIZE : 0) ;
	return ugly_val_set_string(ctx, type_name, "seq", 3) ;
}

static ugly_status t_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	(void)ctx ; (void)rt ;
	((seq *)ref)->lengths++ ;
	*out = 3 ;
	return UGLY_OK ;
}

static ugly_status t_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){
	(void)ctx ; (void)rt ; (void)offset ; (void)remove ; (void)insert ; (void)n ;
	((seq *)ref)->splices++ ;
	return UGLY_OK ;
}

static void t_release(ugly_rt *rt, void *ref){
	(void)rt ; (void)ref ;
}

static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.name = "fixedtest",
	.rt_new = t_rt_new,
	.rt_delete = t_rt_delete,
	.import = t_import,
	.describe = t_describe,
	.length = t_length,
	.splice = t_splice,
	.release = t_release,
} ;


int main(void){
	ugly_ctx *ctx = NULL ;
	ugly_lang *lang = NULL ;
	ugly_rt *rt = NULL ;
	ugly_handle fixed = UGLY_NO_HANDLE, open = UGLY_NO_HANDLE ;
	uint32_t traits = 0 ;
	ugly_val tn = ugly_null() ;

	setvbuf(stdout, NULL, _IOLBF, 0) ;
	ugly_ctx_new(&ctx) ;
	ok((ugly_lang_register(ctx, &plugin, &lang) == UGLY_OK)&&(ugly_rt_new(ctx, lang, &rt) == UGLY_OK),
		"a test language, registered") ;
	ok((ugly_rt_import(ctx, rt, "fixed", &fixed) == UGLY_OK)&&(ugly_rt_import(ctx, rt, "open", &open) == UGLY_OK),
		"two sequences") ;

	ok((ugly_describe(ctx, fixed, &traits, &tn) == UGLY_OK)&&(traits == (UGLY_TRAIT_ELEMENTS | UGLY_TRAIT_FIXED_SIZE)),
		"describe reports FIXED_SIZE") ;
	ugly_val_release(ctx, &tn) ;

	const ugly_error *e = NULL ;
	ok((ugly_splice(ctx, fixed, 0, 0, NULL, 0) == UGLY_ERR_NOT_CAPABLE)&&((e = ugly_last_error(ctx)) != NULL)
		&&(strcmp(ugly_error_language(e), "core") == 0)&&(strstr(ugly_error_message(e), "fixed size") != NULL),
		"splice of a FIXED_SIZE sequence: NOT_CAPABLE, from the core") ;
	ok((fixed_seq.lengths == 0)&&(fixed_seq.splices == 0), "  ... without asking for its length, or calling splice") ;

	ok(ugly_splice(ctx, open, 3, 0, NULL, 0) == UGLY_OK, "splice of another sequence: OK") ;
	ok((open_seq.lengths == 1)&&(open_seq.splices == 1), "  ... after the range check (one length, one splice)") ;
	ok(ugly_splice(ctx, open, 4, 0, NULL, 0) == UGLY_ERR_NOT_FOUND, "  ... which still refuses a range past the end") ;
	ok(open_seq.splices == 1, "  ... without calling splice") ;

	ugly_release(ctx, fixed) ;
	ugly_release(ctx, open) ;
	ok(ugly_rt_delete(ctx, rt) == UGLY_OK, "runtime deleted") ;
	ugly_ctx_delete(ctx) ;
	printf("1..%d\n", n_tests) ;
	return n_failed != 0 ;
}
