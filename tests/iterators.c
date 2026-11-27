/*
 * Iteration and stringification (contract §2, libugly 2.7): ugly_iter,
 * ugly_next and ugly_to_string, exact or refused.
 *
 * Fixtures: count_to(n) makes 0..n-1 lazily; naturals() is 0, 1, 2...
 * without end; fail_after(n) makes 0..n-1, then raises ConformanceError;
 * produced() is how many values the last of them made; sum_iter(xs) and
 * take_iter(xs, n) loop over xs with the language's own loop (the second
 * stopping after n); stringify(x) is the language's own stringification,
 * and a counter's is "counter(<count>)". A plugin without them yet gets a
 * SKIP, and must refuse ugly_iter.
 */
#include "conformance.h"


static int same(ugly_ctx *ctx, ugly_handle a, ugly_handle b){
	bool s = false ;
	return (ugly_same(ctx, a, b, &s) == UGLY_OK) && s ;
}


/* Runs 'it' one value at a time, wanting INT: are they 0..n-1, then the end (twice)? */
static int one_by_one(ugly_ctx *ctx, ugly_handle it, int64_t n){
	int64_t i ;
	for (i = 0 ; i <= n + 1 ; i++){
		ugly_val v ;
		size_t got = 99 ;
		ugly_status st = ugly_next(ctx, it, UGLY_INT, &v, 1, &got) ;
		if ((st != UGLY_OK)||(got != (i < n ? 1u : 0u))||((got == 1)&&(v.as.i != i))){
			diag("  step %lld: %s, got %zu", (long long)i, status_name(st), got) ;
			return 0 ;
		}
	}
	return 1 ;
}


/* The ints in out[0..n) are 0..n-1. */
static int counts(const ugly_val *out, size_t n){
	size_t i ;
	for (i = 0 ; i < n ; i++){
		if ((out[i].kind != UGLY_INT)||(out[i].as.i != (int64_t)i)){
			return 0 ;
		}
	}
	return 1 ;
}


static void release_all(ugly_ctx *ctx, ugly_val *v, size_t n){
	size_t i ;
	for (i = 0 ; i < n ; i++){
		ugly_val_release(ctx, &v[i]) ;
	}
}


/* Core lists: the "c" language's iterator. */
static void core_lists(ugly_ctx *ctx){
	ugly_val items[] = { ugly_int(0), ugly_int(1), ugly_int(2) } ;
	ugly_handle l, it, again ;
	ugly_val v[4], tn ;
	size_t got ;
	uint32_t traits = 0 ;
	if (! is_status(ctx, ugly_list_new(ctx, items, 3, &l), UGLY_OK, "a core list")){
		return ;
	}
	ugly_describe(ctx, l, &traits, &tn) ;
	ugly_val_release(ctx, &tn) ;
	ok(traits & UGLY_TRAIT_ITERABLE, "a core list is ITERABLE") ;
	if (is_status(ctx, ugly_iter(ctx, l, &it), UGLY_OK, "ugly_iter on a core list")){
		ok(one_by_one(ctx, it, 3), "  ... gives 0, 1, 2 one at a time, then the end, twice") ;
		ok((ugly_iter(ctx, it, &again) == UGLY_OK)&&(same(ctx, it, again)), "  ... ugly_iter on the iterator is that iterator") ;
		ugly_release(ctx, again) ;
		ugly_release(ctx, it) ;
	}
	if (ugly_iter(ctx, l, &it) == UGLY_OK){
		ugly_status st = ugly_next(ctx, it, UGLY_DOUBLE, v, 4, &got) ;
		ok((st == UGLY_OK)&&(got == 3)&&(v[2].kind == UGLY_DOUBLE)&&(v[2].as.d == 2.0),
			"a batch of 4 over 3: got 3 (exhausted), converted to the kind wanted") ;
		if (st == UGLY_OK){
			release_all(ctx, v, got) ;
		}
		st = ugly_next(ctx, it, UGLY_INT, v, 0, &got) ;
		is_status(ctx, st, UGLY_ERR_USAGE, "max 0: UGLY_ERR_USAGE") ;
		ugly_release(ctx, it) ;
		st = ugly_next(ctx, it, UGLY_INT, v, 1, &got) ;
		is_status(ctx, st, UGLY_ERR_STALE_HANDLE, "a released iterator: UGLY_ERR_STALE_HANDLE") ;
	}
	is_status(ctx, ugly_next(ctx, l, UGLY_INT, v, 1, &got), UGLY_ERR_NOT_CAPABLE, "ugly_next on a list: UGLY_ERR_NOT_CAPABLE") ;
	is_status(ctx, ugly_to_string(ctx, l, &v[0]), UGLY_ERR_NOT_SUPPORTED, "ugly_to_string on a core list: UGLY_ERR_NOT_SUPPORTED (C has none)") ;
	ugly_handle m ;
	if (ugly_map_new(ctx, &m) == UGLY_OK){
		is_status(ctx, ugly_iter(ctx, m, &it), UGLY_ERR_NOT_CAPABLE, "a core map isn't iterable: UGLY_ERR_NOT_CAPABLE") ;
		ugly_release(ctx, m) ;
	}
	ugly_release(ctx, l) ;
}


/* An iterator over the result of fixture 'name'(arg), or UGLY_NO_HANDLE. */
static ugly_handle iter_of(fixture *f, const char *name, const ugly_val *arg){
	ugly_val g ;
	ugly_handle it = UGLY_NO_HANDLE ;
	if (fcall(f, f->mod, name, arg, (arg != NULL ? 1 : 0), UGLY_HANDLE, &g) == UGLY_OK){
		ugly_iter(f->ctx, g.as.h, &it) ;
		ugly_val_release(f->ctx, &g) ;
	}
	return it ;
}


static void plugin_iterators(fixture *f){
	ugly_ctx *ctx = f->ctx ;
	ugly_val v[8], five = ugly_int(5), two = ugly_int(2), g, tn ;
	ugly_handle it, again ;
	size_t got ;
	ugly_status st = fcall(f, f->mod, "count_to", &five, 1, UGLY_HANDLE, &g) ;
	if (st != UGLY_OK){
		is_status(ctx, st, UGLY_ERR_NOT_FOUND, "no fixture count_to: the plugin doesn't iterate yet") ;
		st = ugly_iter(ctx, f->mod, &it) ;
		ok((st == UGLY_ERR_NOT_IMPLEMENTED)||(st == UGLY_ERR_NOT_CAPABLE), "  ... and refuses ugly_iter") ;
		skip("the plugin's iterators: generators, laziness, batches, errors, its own loop") ;
		return ;
	}
	uint32_t traits = 0 ;
	ugly_describe(ctx, g.as.h, &traits, &tn) ;
	ugly_val_release(ctx, &tn) ;
	ok(traits & UGLY_TRAIT_ITERABLE, "count_to(5) is ITERABLE") ;
	if (is_status(ctx, ugly_iter(ctx, g.as.h, &it), UGLY_OK, "ugly_iter(count_to(5))")){
		ok(one_by_one(ctx, it, 5), "  ... gives 0..4 one at a time, then the end, twice") ;
		ok((ugly_iter(ctx, it, &again) == UGLY_OK)&&(same(ctx, it, again)), "  ... ugly_iter on the iterator is that iterator") ;
		ugly_release(ctx, again) ;
		ugly_release(ctx, it) ;
	}
	ugly_val_release(ctx, &g) ;

	/* Batches. */
	it = iter_of(f, "count_to", &five) ;
	st = ugly_next(ctx, it, UGLY_INT, v, 8, &got) ;
	ok((st == UGLY_OK)&&(got == 5)&&(counts(v, got)), "a batch of 8 over count_to(5): 0..4, and exhausted") ;
	if (st == UGLY_OK){
		release_all(ctx, v, got) ;
	}
	st = ugly_next(ctx, it, UGLY_INT, v, 8, &got) ;
	ok((st == UGLY_OK)&&(got == 0), "  ... and the next batch is empty") ;
	ugly_release(ctx, it) ;

	/* Laziness: nothing runs ahead of what was asked. */
	ugly_val thousand = ugly_int(1000) ;
	it = iter_of(f, "count_to", &thousand) ;
	st = ugly_next(ctx, it, UGLY_INT, v, 3, &got) ;
	ok((st == UGLY_OK)&&(got == 3)&&(counts(v, got)), "count_to(1000), a batch of 3: 0, 1, 2") ;
	ugly_val n ;
	if (fcall(f, f->mod, "produced", NULL, 0, UGLY_INT, &n) == UGLY_OK){
		is_int(n.as.i, 3, "  ... and only 3 were produced") ;
	}
	else {
		skip("no fixture produced(): the language's iterables aren't lazy") ;
	}
	ugly_release(ctx, it) ;

	/* Want: converted by the language's own rules, or refused. */
	it = iter_of(f, "count_to", &five) ;
	st = ugly_next(ctx, it, UGLY_STRING, v, 1, &got) ;
	if (st == UGLY_OK){
		ok((got == 1)&&(v[0].kind == UGLY_STRING)&&(strcmp(sval(&v[0]), "0") == 0), "want STRING: \"0\"") ;
		release_all(ctx, v, got) ;
	}
	else {
		is_status(ctx, st, UGLY_ERR_TYPE, "want STRING: refused (UGLY_ERR_TYPE)") ;
	}
	ugly_release(ctx, it) ;

	/* An infinite one; released early, it's freed (closed). */
	int64_t before = finalized(f, f->mod) ;
	it = iter_of(f, "naturals", NULL) ;
	if (it != UGLY_NO_HANDLE){
		st = ugly_next(ctx, it, UGLY_INT, v, 5, &got) ;
		ok((st == UGLY_OK)&&(got == 5)&&(counts(v, got)), "naturals(), a batch of 5: 0..4") ;
		ugly_release(ctx, it) ;
		ok(finalized(f, f->mod) > before, "  ... released early, it is freed") ;
	}
	else {
		skip("no fixture naturals(): the language has no infinite iterables") ;
	}

	/* Errors: the values before one are delivered, then the error. */
	it = iter_of(f, "fail_after", &two) ;
	st = ugly_next(ctx, it, UGLY_INT, v, 8, &got) ;
	if (is_status(ctx, st, UGLY_ERR_RAISED, "fail_after(2), a batch of 8: UGLY_ERR_RAISED")){
		ok((got == 2)&&(counts(v, got)), "  ... after delivering 0, 1") ;
		release_all(ctx, v, got) ;
		const ugly_error *e = ugly_last_error(ctx) ;
		is_str(e ? ugly_error_type(e) : NULL, "ConformanceError", "  ... a ConformanceError") ;
	}
	ugly_release(ctx, it) ;
	it = iter_of(f, "fail_after", &two) ;
	int64_t i ;
	int steps = 1 ;
	for (i = 0 ; i < 2 ; i++){
		st = ugly_next(ctx, it, UGLY_INT, v, 1, &got) ;
		steps = steps && (st == UGLY_OK) && (got == 1) && (v[0].as.i == i) ;
	}
	ok(steps, "fail_after(2) one at a time: 0, 1 ...") ;
	st = ugly_next(ctx, it, UGLY_INT, v, 1, &got) ;
	ok((st == UGLY_ERR_RAISED)&&(got == 0), "  ... then UGLY_ERR_RAISED, with no value") ;
	ugly_release(ctx, it) ;
	is_status(ctx, ugly_next(ctx, f->mod, UGLY_INT, v, 1, &got), UGLY_ERR_NOT_CAPABLE, "ugly_next on the module: UGLY_ERR_NOT_CAPABLE") ;

	/* The language's own loop, over its own values and over a core list. */
	ugly_val r ;
	if (fcall(f, f->mod, "count_to", &five, 1, UGLY_HANDLE, &g) == UGLY_OK){
		st = fcall(f, f->mod, "sum_iter", &g, 1, UGLY_INT, &r) ;
		ok((st == UGLY_OK)&&(r.as.i == 10), "sum_iter(count_to(5)) with its own loop: 10") ;
		ugly_val_release(ctx, &g) ;
	}
	ugly_val items[] = { ugly_int(1), ugly_int(2), ugly_int(3), ugly_int(4) } ;
	ugly_handle l ;
	if (ugly_list_new(ctx, items, 4, &l) == UGLY_OK){
		ugly_val lv = ugly_hval(l) ;
		st = fcall(f, f->mod, "sum_iter", &lv, 1, UGLY_INT, &r) ;
		ok((st == UGLY_OK)&&(r.as.i == 10), "sum_iter(core list 1..4): its loop over a foreign iterable: 10") ;
		ugly_val args[] = { lv, ugly_int(2) } ;
		st = fcall(f, f->mod, "take_iter", args, 2, UGLY_HANDLE, &r) ;
		if (is_status(ctx, st, UGLY_OK, "take_iter(core list 1..4, 2)")){
			ugly_val e ;
			int64_t len = -1 ;
			ugly_length(ctx, r.as.h, &len) ;
			ok((len == 2)&&(ugly_get_index(ctx, r.as.h, 1, UGLY_INT, &e) == UGLY_OK)&&(e.as.i == 2), "  ... gives 1, 2") ;
			ugly_val_release(ctx, &r) ;
		}
		ugly_release(ctx, l) ;
	}
	ugly_handle nat = iter_of(f, "naturals", NULL) ;
	if (nat != UGLY_NO_HANDLE){
		ugly_val args[] = { ugly_hval(nat), ugly_int(3) } ;
		st = fcall(f, f->mod, "take_iter", args, 2, UGLY_HANDLE, &r) ;
		int64_t len = -1 ;
		ok((st == UGLY_OK)&&(ugly_length(ctx, r.as.h, &len) == UGLY_OK)&&(len == 3), "take_iter(naturals(), 3) stops: 3 values") ;
		if (st == UGLY_OK){
			ugly_val_release(ctx, &r) ;
		}
		ugly_release(ctx, nat) ;
	}
}


static void plugin_to_string(fixture *f){
	ugly_ctx *ctx = f->ctx ;
	ugly_handle c = make_counter(f, f->mod) ;
	ugly_val v ;
	ugly_invoke(ctx, c, "inc", NULL, 0, UGLY_ANY, &v) ;
	ugly_val_release(ctx, &v) ;
	ugly_invoke(ctx, c, "inc", NULL, 0, UGLY_ANY, &v) ;
	ugly_val_release(ctx, &v) ;
	ugly_status st = ugly_to_string(ctx, c, &v) ;
	if (st == UGLY_ERR_NOT_IMPLEMENTED){
		ok(1, "ugly_to_string: not implemented by the plugin yet") ;
		skip("the plugin's stringification") ;
		ugly_release(ctx, c) ;
		return ;
	}
	if (is_status(ctx, st, UGLY_OK, "ugly_to_string(counter)")){
		is_str(sval(&v), "counter(2)", "  ... is its language's: counter(2)") ;
		ugly_val_release(ctx, &v) ;
	}
	ugly_val cv = ugly_hval(c) ;
	st = fcall(f, f->mod, "stringify", &cv, 1, UGLY_STRING, &v) ;
	if (is_status(ctx, st, UGLY_OK, "stringify(counter), in the language")){
		is_str(sval(&v), "counter(2)", "  ... counter(2)") ;
		ugly_val_release(ctx, &v) ;
	}
	ugly_release(ctx, c) ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	core_lists(f.ctx) ;
	plugin_iterators(&f) ;
	plugin_to_string(&f) ;
	fx_teardown(&f) ;
	return done_testing() ;
}
