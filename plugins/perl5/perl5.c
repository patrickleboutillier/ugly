/*
 * The Perl 5 plugin. Each runtime is a perl interpreter (MULTIPLICITY).
 *
 * Lessons from v1 (tag v0.02), applied here:
 * - Every entry makes its interpreter current (PERL_SET_CONTEXT) and puts
 *   the caller's back on the way out.
 * - PERL_SYS_INIT3 runs once per process, only if no perl exists yet (the
 *   host may be perl); PERL_SYS_TERM never runs, because perl can't start
 *   again after it. So the plugin isn't UGLY_CAP_RESTARTABLE.
 *
 * The contract's open questions, as decided for Perl (doc/contract.md):
 * - Calls run in list context and collapse: no value -> null, one -> that
 *   value, several -> an array. scalar(...) on the Perl side asks for
 *   scalar context.
 * - ANY: IOK -> INT, NOK -> DOUBLE, POK -> STRING; booleans via SvIsBOOL
 *   (5.36+) or PL_sv_yes/no's signature. An explicit 'want' numifies only
 *   what looks_like_number.
 * - Strings: UTF-8-flagged -> STRING. Unflagged -> STRING if valid UTF-8
 *   (ASCII, undecoded UTF-8 text), else BYTES; 'want STRING' decodes them
 *   as Latin-1.
 *
 * Native refs are SV* (one reference). Foreign values are blessed
 * Ugly::Proxy objects (a scalar ref holding the handle): &{} calls, @{} and
 * %{} tie to elements and members, AUTOLOAD invokes methods, == compares
 * identity, DESTROY releases. Modules and classes are Ugly::Package objects
 * (a scalar ref holding the package name): their members are subs and
 * nested packages, and new() instantiates.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

#include "ugly_plugin.h"

#ifndef G_LIST
#define G_LIST G_ARRAY     /* G_LIST is the 5.36 name */
#endif


typedef struct pcache_entry {
	ugly_handle h ;
	SV *inner ;                  /* the proxy's referent; not a reference: DESTROY removes it */
	struct pcache_entry *next ;
} pcache_entry ;

typedef struct plrt {
	struct plrt *next ;          /* the registry of live runtimes */
	ugly_rt *urt ;
	PerlInterpreter *perl ;
	int owns ;                   /* created by rt_new; attached ones belong to the host */
	pcache_entry *pcache[64] ;
} plrt ;

static plrt *runtimes ;


static plrt *R(PerlInterpreter *p){
	plrt *r ;
	for (r = runtimes ; (r != NULL)&&(r->perl != p) ; r = r->next){
	}
	return r ;
}


static int registered(plrt *r){
	plrt *x ;
	for (x = runtimes ; (x != NULL)&&(x != r) ; x = x->next){
	}
	return (x != NULL) ;
}


/* Every entry into an interpreter: make it current, and restore the caller's on the way out. */
#define ENTER_PERL(r) PerlInterpreter *saved_perl_ = PERL_GET_CONTEXT ; PerlInterpreter *my_perl = (r)->perl ; PERL_SET_CONTEXT(my_perl)
#define LEAVE_PERL() PERL_SET_CONTEXT(saved_perl_)


/* ---------------------------------------------------------------- */
/* Helpers                                                          */
/* ---------------------------------------------------------------- */

static int valid_utf8(const unsigned char *s, size_t len){
	size_t i = 0 ;
	while (i < len){
		unsigned char c = s[i] ;
		size_t n ;
		uint32_t cp ;
		if (c < 0x80){ i++ ; continue ; }
		else if ((c & 0xE0) == 0xC0){ n = 1 ; cp = c & 0x1F ; }
		else if ((c & 0xF0) == 0xE0){ n = 2 ; cp = c & 0x0F ; }
		else if ((c & 0xF8) == 0xF0){ n = 3 ; cp = c & 0x07 ; }
		else { return 0 ; }
		if (i + n >= len){
			return 0 ;
		}
		size_t k ;
		for (k = 1 ; k <= n ; k++){
			if ((s[i + k] & 0xC0) != 0x80){
				return 0 ;
			}
			cp = (cp << 6) | (s[i + k] & 0x3F) ;
		}
		if ((n == 1 && cp < 0x80)||(n == 2 && cp < 0x800)||(n == 3 && cp < 0x10000)||
			(cp > 0x10FFFF)||((cp >= 0xD800)&&(cp <= 0xDFFF))){
			return 0 ;
		}
		i += n + 1 ;
	}
	return 1 ;
}


static ugly_status fail(ugly_ctx *ctx, plrt *r, ugly_status st, const char *type, const char *msg){
	return ugly_fail(ctx, r->urt, st, type, msg) ;
}


static size_t pc_bucket(ugly_handle h){
	return (size_t)((h * 0x9E3779B97F4A7C15ull) >> 58) ;
}


static SV *pc_find(plrt *r, ugly_handle h){
	pcache_entry *e ;
	for (e = r->pcache[pc_bucket(h)] ; e != NULL ; e = e->next){
		if (e->h == h){
			return e->inner ;
		}
	}
	return NULL ;
}


static void pc_add(plrt *r, ugly_handle h, SV *inner){
	pcache_entry *e = malloc(sizeof(pcache_entry)) ;
	if (e != NULL){
		e->h = h ;
		e->inner = inner ;
		e->next = r->pcache[pc_bucket(h)] ;
		r->pcache[pc_bucket(h)] = e ;
	}
}


static void pc_remove(plrt *r, ugly_handle h, SV *inner){
	pcache_entry **p ;
	for (p = &r->pcache[pc_bucket(h)] ; *p != NULL ; p = &(*p)->next){
		if (((*p)->h == h)&&((*p)->inner == inner)){
			pcache_entry *e = *p ;
			*p = e->next ;
			free(e) ;
			return ;
		}
	}
}


/* A blessed scalar ref of class 'cls' holding 'name' or a handle: our own object kinds. */
static int is_a(pTHX_ SV *sv, const char *cls){
	if ((! SvROK(sv))||(! SvOBJECT(SvRV(sv)))){
		return 0 ;
	}
	const char *name = HvNAME(SvSTASH(SvRV(sv))) ;
	return (name != NULL)&&(strcmp(name, cls) == 0) ;
}

#define IS_PROXY(sv) is_a(aTHX_ (sv), "Ugly::Proxy")
#define IS_PACKAGE(sv) is_a(aTHX_ (sv), "Ugly::Package")
#define IS_ERROR(sv) is_a(aTHX_ (sv), "Ugly::Error")
#define PROXY_HANDLE(sv) ((ugly_handle)SvUV(SvRV(sv)))
#define PACKAGE_NAME(sv) (SvPV_nolen(SvRV(sv)))


static SV *new_package(pTHX_ const char *name){
	SV *rv = newRV_noinc(newSVpv(name, 0)) ;
	sv_bless(rv, gv_stashpv("Ugly::Package", GV_ADD)) ;
	return rv ;
}


/* ---------------------------------------------------------------- */
/* Perl -> ugly_val                                                 */
/* ---------------------------------------------------------------- */

static uintptr_t identity_of(pTHX_ SV *sv){
	if (! SvROK(sv)){
		return 0 ;
	}
	if (IS_PACKAGE(sv)){
		HV *stash = gv_stashpv(PACKAGE_NAME(sv), 0) ;
		return (stash != NULL ? (uintptr_t)stash : (uintptr_t)SvRV(sv)) ;
	}
	return (uintptr_t)SvRV(sv) ;
}


/* Exports a Perl value as an owned handle; our own proxies unwrap to their handle (rule 5). */
static ugly_status export_sv(pTHX_ ugly_ctx *ctx, plrt *r, SV *sv, ugly_handle *out){
	if (IS_PROXY(sv)){
		return ugly_dup(ctx, PROXY_HANDLE(sv), out) ;
	}
	return ugly_export(ctx, r->urt, newSVsv(sv), identity_of(aTHX_ sv), out) ;
}


static ugly_status type_error(ugly_ctx *ctx, plrt *r, const char *to){
	char msg[96] ;
	snprintf(msg, sizeof(msg), "can't convert this Perl value to %s", to) ;
	return fail(ctx, r, UGLY_ERR_TYPE, "TypeError", msg) ;
}


static int is_bool(pTHX_ SV *sv){
#ifdef SvIsBOOL
	return SvIsBOOL(sv) ;
#else
	/* PL_sv_yes and PL_sv_no (and their copies) are the only values with all three flags and "1"/"". */
	if (SvIOK(sv) && SvNOK(sv) && SvPOK(sv) && (! SvUTF8(sv))){
		STRLEN len ;
		const char *s = SvPV(sv, len) ;
		IV i = SvIV(sv) ;
		return ((i == 1)&&(len == 1)&&(s[0] == '1'))||((i == 0)&&(len == 0)) ;
	}
	return 0 ;
#endif
}


/* The string of a non-reference SV as UTF-8: flagged strings as they are, unflagged ones as Latin-1. */
static ugly_status string_val(pTHX_ ugly_ctx *ctx, plrt *r, SV *sv, int latin1, ugly_val *out){
	STRLEN len ;
	const char *s = SvPV(sv, len) ;
	if (SvUTF8(sv)||(! latin1)){
		return (valid_utf8((const unsigned char *)s, len)
			? ugly_val_set_string(ctx, out, s, len)
			: fail(ctx, r, UGLY_ERR_TYPE, "TypeError", "string isn't valid UTF-8")) ;
	}
	SV *copy = sv_2mortal(newSVpvn(s, len)) ;
	sv_utf8_upgrade(copy) ;
	s = SvPV(copy, len) ;
	return ugly_val_set_string(ctx, out, s, len) ;
}


static ugly_status to_val(pTHX_ ugly_ctx *ctx, plrt *r, SV *sv, ugly_kind want, ugly_val *out){
	*out = ugly_null() ;
	SvGETMAGIC(sv) ;
	if (want == UGLY_ANY){
		if (! SvOK(sv)){
			return UGLY_OK ;
		}
		if (SvROK(sv)){
			want = UGLY_HANDLE ;
		}
		else if (is_bool(aTHX_ sv)){
			want = UGLY_BOOL ;
		}
		else if (SvIOK(sv)){
			want = UGLY_INT ;
		}
		else if (SvNOK(sv)){
			want = UGLY_DOUBLE ;
		}
		else {
			/* An unflagged string: text if it's valid UTF-8 (ASCII, undecoded UTF-8), else bytes. */
			STRLEN len ;
			const char *s = SvPV(sv, len) ;
			want = ((SvUTF8(sv)||valid_utf8((const unsigned char *)s, len)) ? UGLY_STRING : UGLY_BYTES) ;
			if (want == UGLY_STRING){
				return string_val(aTHX_ ctx, r, sv, 0, out) ;
			}
		}
	}
	switch (want){
		case UGLY_NULL:
			return (SvOK(sv) ? type_error(ctx, r, "null") : UGLY_OK) ;
		case UGLY_BOOL:
			*out = ugly_bool(SvTRUE(sv)) ;
			return UGLY_OK ;
		case UGLY_INT: {
			if (SvROK(sv)||(! SvOK(sv))){
				return type_error(ctx, r, "an integer") ;
			}
			if (SvIOK(sv)){
				if (SvIsUV(sv)&&(SvUV(sv) > (UV)INT64_MAX)){
					return fail(ctx, r, UGLY_ERR_TYPE, "TypeError", "integer doesn't fit in int64") ;
				}
				*out = ugly_int((int64_t)SvIV(sv)) ;
				return UGLY_OK ;
			}
			if (SvNOK(sv)||looks_like_number(sv)){
				NV d = SvNV(sv) ;
				if ((d == (NV)(IV)d)&&(d >= -9223372036854775808.0)&&(d < 9223372036854775808.0)){
					*out = ugly_int((int64_t)d) ;
					return UGLY_OK ;
				}
			}
			return type_error(ctx, r, "an integer") ;
		}
		case UGLY_DOUBLE:
			if (SvROK(sv)||(! SvOK(sv))||(! (SvIOK(sv)||SvNOK(sv)||looks_like_number(sv)))){
				return type_error(ctx, r, "a double") ;
			}
			*out = ugly_double((double)SvNV(sv)) ;
			return UGLY_OK ;
		case UGLY_STRING:
			if (SvROK(sv)||(! SvOK(sv))){
				return type_error(ctx, r, "a string") ;
			}
			return string_val(aTHX_ ctx, r, sv, 1, out) ;
		case UGLY_BYTES: {
			if (SvROK(sv)||(! SvOK(sv))){
				return type_error(ctx, r, "bytes") ;
			}
			SV *copy = sv_2mortal(newSVsv(sv)) ;
			if (SvUTF8(copy)&&(! sv_utf8_downgrade(copy, TRUE))){
				return fail(ctx, r, UGLY_ERR_TYPE, "TypeError", "string has wide characters") ;
			}
			STRLEN len ;
			const char *s = SvPV(copy, len) ;
			return ugly_val_set_bytes(ctx, out, (const uint8_t *)s, len) ;
		}
		case UGLY_HANDLE: {
			if (! SvROK(sv)){
				return type_error(ctx, r, "a handle") ;
			}
			ugly_handle h ;
			ugly_status st = export_sv(aTHX_ ctx, r, sv, &h) ;
			if (st == UGLY_OK){
				*out = ugly_hval(h) ;
			}
			return st ;
		}
		default:
			return UGLY_ERR_USAGE ;
	}
}


/* ---------------------------------------------------------------- */
/* ugly_val -> Perl                                                 */
/* ---------------------------------------------------------------- */

static void croak_ugly(pTHX_ ugly_ctx *ctx, plrt *r, ugly_status st) ;

static SV *proxy_for(pTHX_ ugly_ctx *ctx, plrt *r, ugly_handle borrowed, ugly_status *st){
	ugly_handle h ;
	*st = ugly_dup(ctx, borrowed, &h) ;     /* the owned handle: the same id for the same value */
	if (*st != UGLY_OK){
		return NULL ;
	}
	SV *inner = pc_find(r, h) ;
	if (inner != NULL){
		ugly_release(ctx, h) ;     /* the cached proxy holds its own reference */
		return newRV_inc(inner) ;
	}
	inner = newSVuv((UV)h) ;
	SV *rv = newRV_noinc(inner) ;
	sv_bless(rv, gv_stashpv("Ugly::Proxy", GV_ADD)) ;
	pc_add(r, h, inner) ;
	return rv ;
}


/* An argument (borrowed) as a new SV: ours resolve to the original (rule 5), foreign ones get a proxy. */
static SV *from_val(pTHX_ ugly_ctx *ctx, plrt *r, ugly_val v, ugly_status *st){
	*st = UGLY_OK ;
	switch (v.kind){
		case UGLY_NULL: return newSV(0) ;
		case UGLY_BOOL: return newSVsv(v.as.b ? &PL_sv_yes : &PL_sv_no) ;
		case UGLY_INT: return newSViv((IV)v.as.i) ;
		case UGLY_DOUBLE: return newSVnv((NV)v.as.d) ;
		case UGLY_STRING: {
			SV *sv = newSVpvn(v.as.str.ptr, v.as.str.len) ;
			SvUTF8_on(sv) ;
			return sv ;
		}
		case UGLY_BYTES: return newSVpvn((const char *)v.as.bytes.ptr, v.as.bytes.len) ;
		case UGLY_HANDLE: {
			void *ref = NULL ;
			*st = ugly_resolve(ctx, r->urt, v.as.h, &ref) ;
			if (*st != UGLY_OK){
				return NULL ;
			}
			if (ref != NULL){
				return newSVsv((SV *)ref) ;
			}
			return proxy_for(aTHX_ ctx, r, v.as.h, st) ;
		}
		default:
			*st = UGLY_ERR_USAGE ;
			return NULL ;
	}
}


/* ---------------------------------------------------------------- */
/* Errors at the boundary                                           */
/* ---------------------------------------------------------------- */

/*
 * Dies with the ctx's current error: the original value if its payload is
 * ours coming home (rule 7), otherwise an Ugly::Error object. Doesn't return.
 */
static void croak_ugly(pTHX_ ugly_ctx *ctx, plrt *r, ugly_status st){
	if (ugly_last_error(ctx) == NULL){
		ugly_fail(ctx, r->urt, st, "Error", "operation failed") ;
	}
	ugly_error *err = ugly_take_error(ctx) ;
	if (err == NULL){
		croak("ugly: operation failed") ;
	}
	ugly_handle payload = UGLY_NO_HANDLE ;
	ugly_error_payload(ctx, err, &payload) ;
	void *ref = NULL ;
	if ((payload != UGLY_NO_HANDLE)&&(ugly_resolve(ctx, r->urt, payload, &ref) == UGLY_OK)&&(ref != NULL)){
		/* Take our copy first: releasing the error may free the SV 'ref' points to. */
		SV *original = sv_2mortal(newSVsv((SV *)ref)) ;
		ugly_release(ctx, payload) ;
		ugly_error_free(err) ;
		croak_sv(original) ;
	}
	HV *hv = newHV() ;
	hv_stores(hv, "type", newSVpv(ugly_error_type(err), 0)) ;
	hv_stores(hv, "message", newSVpv(ugly_error_message(err), 0)) ;
	hv_stores(hv, "language", newSVpv(ugly_error_language(err), 0)) ;
	SV *pv = NULL ;
	if (payload != UGLY_NO_HANDLE){
		ugly_status pst ;
		pv = from_val(aTHX_ ctx, r, ugly_hval(payload), &pst) ;
		ugly_release(ctx, payload) ;
	}
	hv_stores(hv, "payload", (pv != NULL ? pv : newSV(0))) ;
	hv_stores(hv, "_err", newSVuv((UV)(uintptr_t)err)) ;
	SV *e = sv_2mortal(newRV_noinc((SV *)hv)) ;
	sv_bless(e, gv_stashpv("Ugly::Error", GV_ADD)) ;
	croak_sv(e) ;
}


/* $@ after a failed call, escaping to the core (rule 7). Clears it. */
static ugly_status perl_failure(pTHX_ ugly_ctx *ctx, plrt *r){
	SV *e = sv_2mortal(newSVsv(ERRSV)) ;
	sv_setsv(ERRSV, &PL_sv_undef) ;

	/* Our own error object, not yet consumed: the original error, unchanged. */
	if (IS_ERROR(e)){
		SV **p = hv_fetchs((HV *)SvRV(e), "_err", 0) ;
		if ((p != NULL)&&(SvOK(*p))){
			ugly_error *err = (ugly_error *)(uintptr_t)SvUV(*p) ;
			sv_setsv(*p, &PL_sv_undef) ;
			return ugly_restore_error(ctx, err) ;
		}
	}
	/* die $proxy: the payload is the foreign value itself. */
	if (IS_PROXY(e)){
		ugly_handle h ;
		ugly_status st = ugly_dup(ctx, PROXY_HANDLE(e), &h) ;
		return ugly_raise_handle(ctx, r->urt, "ForeignError", "rethrown foreign value", (st == UGLY_OK ? h : UGLY_NO_HANDLE)) ;
	}

	/* An object: its class and its message field; a string: "die" and the string. */
	char type[128] = "die", message[512] = "" ;
	if (SvROK(e)&&SvOBJECT(SvRV(e))){
		snprintf(type, sizeof(type), "%s", HvNAME(SvSTASH(SvRV(e)))) ;
		if (SvTYPE(SvRV(e)) == SVt_PVHV){
			SV **m = hv_fetchs((HV *)SvRV(e), "message", 0) ;
			if ((m != NULL)&&(SvOK(*m))){
				snprintf(message, sizeof(message), "%s", SvPV_nolen(*m)) ;
			}
		}
	}
	if (message[0] == '\0'){
		snprintf(message, sizeof(message), "%s", SvPV_nolen(e)) ;
	}
	return ugly_raise_native(ctx, r->urt, type, message, newSVsv(e), identity_of(aTHX_ e)) ;
}


/* ---------------------------------------------------------------- */
/* Calls                                                            */
/* ---------------------------------------------------------------- */

/*
 * Calls 'fn' (a CV or a method name) with an optional invocant and args, in
 * list context, collapsing the result: none -> null, one -> it, more -> an
 * array. The result is converted with 'want'.
 */
/*
 * Calls fn (or method on invocant) with args, then the call options' keyword
 * arguments as trailing name => value pairs (contract §11), in the context
 * they ask for: list (the default, collapsed as decision 9 says), scalar or
 * void. A hint is refused.
 */
static ugly_status call_perl(pTHX_ ugly_ctx *ctx, plrt *r, SV *fn, const char *method, SV *invocant,
	const ugly_val *args, size_t nargs, const ugly_opts *o, ugly_kind want, ugly_val *out){

	I32 gimme = G_LIST ;
	size_t nkw = 0 ;
	if (o != NULL){
		if (o->hint != NULL){
			return fail(ctx, r, UGLY_ERR_NOT_SUPPORTED, "Error", "Perl takes no call hint") ;
		}
		if (o->context == NULL || strcmp(o->context, "list") == 0){
			gimme = G_LIST ;
		}
		else if (strcmp(o->context, "scalar") == 0){
			gimme = G_SCALAR ;
		}
		else if (strcmp(o->context, "void") == 0){
			gimme = G_VOID ;
		}
		else {
			return fail(ctx, r, UGLY_ERR_USAGE, "Error", "a call context is list, scalar or void") ;
		}
		nkw = o->nkwargs ;
	}
	ENTER ;
	SAVETMPS ;
	/* Convert every argument before touching the stack, so a failure leaves no mark behind. */
	size_t total = nargs + 2 * nkw ;
	SV **svs = calloc(total + 1, sizeof(SV *)) ;
	size_t i ;
	ugly_status st = UGLY_OK ;
	for (i = 0 ; (i < nargs)&&(st == UGLY_OK) ; i++){
		svs[i] = from_val(aTHX_ ctx, r, args[i], &st) ;
		if (svs[i] != NULL){
			sv_2mortal(svs[i]) ;
		}
	}
	for (i = 0 ; (i < nkw)&&(st == UGLY_OK) ; i++){
		svs[nargs + 2 * i] = sv_2mortal(newSVpvn_utf8(o->kwargs[i].name, strlen(o->kwargs[i].name), 1)) ;
		svs[nargs + 2 * i + 1] = from_val(aTHX_ ctx, r, o->kwargs[i].value, &st) ;
		if (svs[nargs + 2 * i + 1] != NULL){
			sv_2mortal(svs[nargs + 2 * i + 1]) ;
		}
	}
	if (st != UGLY_OK){
		free(svs) ;
		FREETMPS ;
		LEAVE ;
		return st ;
	}
	dSP ;
	PUSHMARK(SP) ;
	if (invocant != NULL){
		XPUSHs(invocant) ;
	}
	for (i = 0 ; i < total ; i++){
		XPUSHs(svs[i]) ;
	}
	PUTBACK ;
	free(svs) ;
	int count = (method != NULL ? call_method(method, gimme | G_EVAL) : call_sv(fn, gimme | G_EVAL)) ;
	SPAGAIN ;
	if (SvTRUE(ERRSV)){
		SP -= count ;
		PUTBACK ;
		st = perl_failure(aTHX_ ctx, r) ;
	}
	else {
		SV *result ;
		if (count == 0){
			result = sv_2mortal(newSV(0)) ;
		}
		else if (count == 1){
			result = POPs ;
		}
		else {
			AV *av = newAV() ;
			av_extend(av, count - 1) ;
			int k ;
			for (k = count - 1 ; k >= 0 ; k--){
				av_store(av, k, newSVsv(POPs)) ;
			}
			result = sv_2mortal(newRV_noinc((SV *)av)) ;
		}
		PUTBACK ;
		st = to_val(aTHX_ ctx, r, result, want, out) ;
	}
	FREETMPS ;
	LEAVE ;
	return st ;
}


/* ---------------------------------------------------------------- */
/* XS functions behind Ugly::Proxy, Ugly::Error and Ugly            */
/* ---------------------------------------------------------------- */

static ugly_ctx *xs_ctx(pTHX){
	ugly_ctx *ctx = ugly_current_ctx() ;
	if (ctx == NULL){
		croak("ugly: no ctx on this thread") ;
	}
	return ctx ;
}


/* Perl args from 'first' -> owned ugly_vals. */
static ugly_val *xs_args(pTHX_ ugly_ctx *ctx, plrt *r, SV **sp_args, int n){
	ugly_val *a = calloc((size_t)n + 1, sizeof(ugly_val)) ;
	int i ;
	for (i = 0 ; i < n ; i++){
		ugly_status st = to_val(aTHX_ ctx, r, sp_args[i], UGLY_ANY, &a[i]) ;
		if (st != UGLY_OK){
			int j ;
			for (j = 0 ; j < i ; j++){
				ugly_val_release(ctx, &a[j]) ;
			}
			free(a) ;
			croak_ugly(aTHX_ ctx, r, st) ;
		}
	}
	return a ;
}


static void free_args(ugly_ctx *ctx, ugly_val *a, int n){
	int i ;
	for (i = 0 ; i < n ; i++){
		ugly_val_release(ctx, &a[i]) ;
	}
	free(a) ;
}


/* A core result as a mortal SV (and releases it), or dies. */
static SV *xs_result(pTHX_ ugly_ctx *ctx, plrt *r, ugly_status st, ugly_val *out){
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	SV *sv = from_val(aTHX_ ctx, r, *out, &st) ;
	ugly_val_release(ctx, out) ;
	if (sv == NULL){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	return sv_2mortal(sv) ;
}


/* Ugly::Proxy::_call($proxy, @args) */
/*
 * Keyword arguments (contract §11): Ugly::kw(name => value, ...) as the last
 * argument, an Ugly::Kw object holding the pairs in order. If it's there,
 * *n shrinks by one and k holds them (names point into the object, which
 * is on the stack; values are owned: free_kw).
 */
typedef struct { ugly_opts o ; ugly_kwarg *kw ; } perl_kw ;

static void free_kw(ugly_ctx *ctx, perl_kw *k) ;

static void take_kw(pTHX_ ugly_ctx *ctx, plrt *r, SV **args, int *n, perl_kw *k){
	k->o = (ugly_opts)UGLY_OPTS_INIT ;
	k->kw = NULL ;
	if ((*n == 0)||(! sv_isobject(args[*n - 1]))||(! sv_derived_from(args[*n - 1], "Ugly::Kw"))){
		return ;
	}
	AV *av = (AV *)SvRV(args[--(*n)]) ;
	SSize_t len = av_len(av) + 1, i ;
	if (len % 2 != 0){
		croak("Ugly::kw takes name => value pairs") ;
	}
	k->kw = calloc((size_t)len / 2 + 1, sizeof(ugly_kwarg)) ;
	for (i = 0 ; i < len ; i += 2){
		SV **name = av_fetch(av, i, 0), **value = av_fetch(av, i + 1, 0) ;
		ugly_kwarg *kw = &k->kw[k->o.nkwargs] ;
		kw->name = (name != NULL ? SvPVutf8_nolen(*name) : "") ;
		ugly_status st = (value != NULL ? to_val(aTHX_ ctx, r, *value, UGLY_ANY, &kw->value) : UGLY_OK) ;
		if (st != UGLY_OK){
			free_kw(ctx, k) ;
			croak_ugly(aTHX_ ctx, r, st) ;
		}
		k->o.nkwargs++ ;
	}
	k->o.kwargs = k->kw ;
}


static void free_kw(ugly_ctx *ctx, perl_kw *k){
	size_t i ;
	for (i = 0 ; i < k->o.nkwargs ; i++){
		ugly_val_release(ctx, &k->kw[i].value) ;
	}
	free(k->kw) ;
	k->kw = NULL ;
	k->o.nkwargs = 0 ;
}


XS(xs_proxy_call){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 1)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_handle h = PROXY_HANDLE(ST(0)) ;
	int n = items - 1 ;
	perl_kw k ;
	take_kw(aTHX_ ctx, r, &ST(1), &n, &k) ;
	ugly_val *a = xs_args(aTHX_ ctx, r, &ST(1), n) ;
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_call_opts(ctx, h, a, (size_t)n, &k.o, UGLY_ANY, &out) ;
	free_args(ctx, a, n) ;
	free_kw(ctx, &k) ;
	ST(0) = xs_result(aTHX_ ctx, r, st, &out) ;
	XSRETURN(1) ;
}


/* Ugly::Proxy::_invoke($proxy, $method, @args) */
XS(xs_proxy_invoke){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 2)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_handle h = PROXY_HANDLE(ST(0)) ;
	const char *method = SvPV_nolen(ST(1)) ;
	int n = items - 2 ;
	perl_kw k ;
	take_kw(aTHX_ ctx, r, &ST(2), &n, &k) ;
	ugly_val *a = xs_args(aTHX_ ctx, r, &ST(2), n) ;
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_invoke_opts(ctx, h, method, a, (size_t)n, &k.o, UGLY_ANY, &out) ;
	free_args(ctx, a, n) ;
	free_kw(ctx, &k) ;
	ST(0) = xs_result(aTHX_ ctx, r, st, &out) ;
	XSRETURN(1) ;
}


/* Ugly::Proxy::_get($proxy, $key): an element for an integer key on an element collection, else a member. */
XS(xs_proxy_get){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 3)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_handle h = PROXY_HANDLE(ST(0)) ;
	ugly_val out = ugly_null() ;
	ugly_status st ;
	if (SvTRUE(ST(2))){
		st = ugly_get_index(ctx, h, (int64_t)SvIV(ST(1)), UGLY_ANY, &out) ;
	}
	else {
		st = ugly_get_member(ctx, h, SvPV_nolen(ST(1)), UGLY_ANY, &out) ;
	}
	if (st == UGLY_ERR_NOT_FOUND){
		XSRETURN_UNDEF ;
	}
	ST(0) = xs_result(aTHX_ ctx, r, st, &out) ;
	XSRETURN(1) ;
}


/* Ugly::Proxy::_set($proxy, $key, $value, $is_index) */
XS(xs_proxy_set){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 4)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_handle h = PROXY_HANDLE(ST(0)) ;
	ugly_val v ;
	ugly_status st = to_val(aTHX_ ctx, r, ST(2), UGLY_ANY, &v) ;
	if (st == UGLY_OK){
		st = (SvTRUE(ST(3)) ? ugly_set_index(ctx, h, (int64_t)SvIV(ST(1)), v)
			: ugly_set_member(ctx, h, SvPV_nolen(ST(1)), v)) ;
		ugly_val_release(ctx, &v) ;
	}
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	XSRETURN_EMPTY ;
}


/* Ugly::Proxy::_has($proxy, $name): whether the member exists (even if it is undef). */
XS(xs_proxy_has){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 2)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_get_member(ctx, PROXY_HANDLE(ST(0)), SvPV_nolen(ST(1)), UGLY_ANY, &out) ;
	ugly_val_release(ctx, &out) ;
	if ((st == UGLY_ERR_NOT_FOUND)||(st == UGLY_ERR_NOT_CAPABLE)){
		XSRETURN_NO ;
	}
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	XSRETURN_YES ;
}


/* Ugly::Proxy::_delete($proxy, $name) */
XS(xs_proxy_delete){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 2)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_status st = ugly_delete_member(ctx, PROXY_HANDLE(ST(0)), SvPV_nolen(ST(1))) ;
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	XSRETURN_EMPTY ;
}


/* Ugly::Proxy::_splice($proxy, $offset, $remove, @insert) */
XS(xs_proxy_splice){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 3)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	int64_t off = (int64_t)SvIV(ST(1)), rm = (int64_t)SvIV(ST(2)) ;
	ugly_val *a = xs_args(aTHX_ ctx, r, &ST(3), items - 3) ;
	ugly_status st = ugly_splice(ctx, PROXY_HANDLE(ST(0)), off, rm, a, (size_t)(items - 3)) ;
	free_args(ctx, a, items - 3) ;
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	XSRETURN_EMPTY ;
}


/* Ugly::Proxy::_new($class_proxy, @args) */
XS(xs_proxy_new){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 1)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	int n = items - 1 ;
	perl_kw k ;
	take_kw(aTHX_ ctx, r, &ST(1), &n, &k) ;
	ugly_val *a = xs_args(aTHX_ ctx, r, &ST(1), n) ;
	ugly_handle h = UGLY_NO_HANDLE ;
	ugly_status st = ugly_new_opts(ctx, PROXY_HANDLE(ST(0)), a, (size_t)n, &k.o, &h) ;
	free_args(ctx, a, n) ;
	free_kw(ctx, &k) ;
	ugly_val out = ugly_hval(h) ;
	ST(0) = xs_result(aTHX_ ctx, r, st, &out) ;
	XSRETURN(1) ;
}


/* Ugly::Proxy::_length($proxy) */
XS(xs_proxy_length){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 1)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	int64_t n = 0 ;
	ugly_status st = ugly_length(ctx, PROXY_HANDLE(ST(0)), &n) ;
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	ST(0) = sv_2mortal(newSViv((IV)n)) ;
	XSRETURN(1) ;
}


/* Ugly::Proxy::_keys($proxy): the member names. */
XS(xs_proxy_keys){
	dXSARGS ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 1)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_handle names ;
	if (ugly_member_names(ctx, PROXY_HANDLE(ST(0)), &names) != UGLY_OK){
		XSRETURN_EMPTY ;
	}
	int64_t n = 0, i ;
	ugly_length(ctx, names, &n) ;
	SP -= items ;
	for (i = 0 ; i < n ; i++){
		ugly_val s ;
		if (ugly_get_index(ctx, names, i, UGLY_STRING, &s) == UGLY_OK){
			SV *k = newSVpvn(s.as.str.ptr, s.as.str.len) ;
			SvUTF8_on(k) ;
			XPUSHs(sv_2mortal(k)) ;
			ugly_val_release(ctx, &s) ;
		}
	}
	ugly_release(ctx, names) ;
	PUTBACK ;
}


/* Ugly::Proxy::_traits($proxy) */
XS(xs_proxy_traits){
	dXSARGS ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	uint32_t traits = 0 ;
	ugly_val tn ;
	if ((items >= 1)&&IS_PROXY(ST(0))&&(ugly_describe(ctx, PROXY_HANDLE(ST(0)), &traits, &tn) == UGLY_OK)){
		ugly_val_release(ctx, &tn) ;
	}
	ST(0) = sv_2mortal(newSVuv(traits)) ;
	XSRETURN(1) ;
}


/* Ugly::Proxy::_same($a, $b) */
XS(xs_proxy_same){
	dXSARGS ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	bool same = false ;
	if ((items >= 2)&&IS_PROXY(ST(0))&&IS_PROXY(ST(1))){
		ugly_same(ctx, PROXY_HANDLE(ST(0)), PROXY_HANDLE(ST(1)), &same) ;
	}
	ST(0) = boolSV(same) ;
	XSRETURN(1) ;
}


/* Ugly::Proxy::_release($proxy): from DESTROY. */
XS(xs_proxy_release){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	if ((items >= 1)&&IS_PROXY(ST(0))){
		SV *inner = SvRV(ST(0)) ;
		ugly_handle h = (ugly_handle)SvUV(inner) ;
		if (h != UGLY_NO_HANDLE){
			if ((r != NULL)&&registered(r)){
				pc_remove(r, h, inner) ;
			}
			ugly_release(ugly_current_ctx(), h) ;
			sv_setuv(inner, 0) ;
		}
	}
	XSRETURN_EMPTY ;
}


/* Ugly::Error::_free($error): from DESTROY, for an error that never escaped. */
XS(xs_error_free){
	dXSARGS ;
	if ((items >= 1)&&IS_ERROR(ST(0))){
		SV **p = hv_fetchs((HV *)SvRV(ST(0)), "_err", 0) ;
		if ((p != NULL)&&SvOK(*p)){
			ugly_error_free((ugly_error *)(uintptr_t)SvUV(*p)) ;
			sv_setsv(*p, &PL_sv_undef) ;
		}
	}
	XSRETURN_EMPTY ;
}


/* Ugly::module($lang, $name) and Ugly::eval($lang, $code): another language's module or code. */
static ugly_rt *other(pTHX_ ugly_ctx *ctx, plrt *r, const char *lang){
	if (strcmp(lang, "perl5") == 0){
		croak("ugly: use require and eval for Perl itself") ;
	}
	ugly_rt *rt ;
	ugly_status st = ugly_rt_get(ctx, lang, &rt) ;
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	return rt ;
}

XS(xs_module){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if (items < 2){
		croak("usage: Ugly::module(lang, name)") ;
	}
	ugly_rt *rt = other(aTHX_ ctx, r, SvPV_nolen(ST(0))) ;
	ugly_handle h ;
	ugly_status st = ugly_rt_import(ctx, rt, SvPV_nolen(ST(1)), &h) ;
	ugly_val v = ugly_hval(h) ;
	ST(0) = xs_result(aTHX_ ctx, r, st, &v) ;
	XSRETURN(1) ;
}

/* Ugly::_configure(lang, key, value) */
XS(xs_configure){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if (items < 3){
		croak("usage: Ugly::_configure(lang, key, value)") ;
	}
	ugly_status st = ugly_configure(ctx, SvPV_nolen(ST(0)), SvPV_nolen(ST(1)), SvPV_nolen(ST(2))) ;
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	XSRETURN_EMPTY ;
}

XS(xs_eval){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if (items < 2){
		croak("usage: Ugly::eval(lang, code)") ;
	}
	ugly_rt *rt = other(aTHX_ ctx, r, SvPV_nolen(ST(0))) ;
	STRLEN len ;
	const char *code = SvPV(ST(1), len) ;
	ugly_val out = ugly_null() ;
	ugly_status st = ugly_rt_eval(ctx, rt, code, len, UGLY_ANY, &out) ;
	ST(0) = xs_result(aTHX_ ctx, r, st, &out) ;
	XSRETURN(1) ;
}


/* The Perl side of proxies, errors, packages and imports (helper.pm), loaded into every runtime. */
#include "helper_pm.h"


EXTERN_C void boot_DynaLoader(pTHX_ CV *cv) ;

static void register_xs(pTHX){
	const char *file = __FILE__ ;
	newXS("Ugly::Proxy::_call", xs_proxy_call, file) ;
	newXS("Ugly::Proxy::_invoke", xs_proxy_invoke, file) ;
	newXS("Ugly::Proxy::_get", xs_proxy_get, file) ;
	newXS("Ugly::Proxy::_set", xs_proxy_set, file) ;
	newXS("Ugly::Proxy::_length", xs_proxy_length, file) ;
	newXS("Ugly::Proxy::_has", xs_proxy_has, file) ;
	newXS("Ugly::Proxy::_delete", xs_proxy_delete, file) ;
	newXS("Ugly::Proxy::_splice", xs_proxy_splice, file) ;
	newXS("Ugly::Proxy::_new", xs_proxy_new, file) ;
	newXS("Ugly::Proxy::_keys", xs_proxy_keys, file) ;
	newXS("Ugly::Proxy::_traits", xs_proxy_traits, file) ;
	newXS("Ugly::Proxy::_same", xs_proxy_same, file) ;
	newXS("Ugly::Proxy::_release", xs_proxy_release, file) ;
	newXS("Ugly::Error::_free", xs_error_free, file) ;
	newXS("Ugly::module", xs_module, file) ;
	newXS("Ugly::eval", xs_eval, file) ;
	newXS("Ugly::_configure", xs_configure, file) ;
}


static void xs_init(pTHX){
	newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, __FILE__) ;
	register_xs(aTHX) ;
}


/* Defines the helper packages (once per interpreter). 0 on success. */
static int load_helper(pTHX){
	eval_pv(helper_pm, FALSE) ;
	return (SvTRUE(ERRSV) ? -1 : 0) ;
}


/* ---------------------------------------------------------------- */
/* Operations                                                       */
/* ---------------------------------------------------------------- */

static plrt *RT(ugly_rt *rt){
	return ugly_rt_native(rt) ;
}


/* A nested package's name (pkg::name) if it exists, as a new string; NULL otherwise. */
static char *nested_package(pTHX_ const char *pkg, const char *name){
	size_t len = strlen(pkg) + strlen(name) + 3 ;
	char *full = malloc(len) ;
	if (full == NULL){
		return NULL ;
	}
	if (strcmp(pkg, "main") == 0){
		snprintf(full, len, "%s", name) ;
	}
	else {
		snprintf(full, len, "%s::%s", pkg, name) ;
	}
	if (gv_stashpv(full, 0) == NULL){
		free(full) ;
		return NULL ;
	}
	return full ;
}


/* The sub pkg::name, or NULL. */
static CV *package_sub(pTHX_ const char *pkg, const char *name){
	HV *stash = gv_stashpv(pkg, 0) ;
	if (stash == NULL){
		return NULL ;
	}
	SV **gvp = hv_fetch(stash, name, (I32)strlen(name), 0) ;
	if ((gvp == NULL)||(! isGV(*gvp))){
		return NULL ;
	}
	return GvCV((GV *)*gvp) ;
}


static ugly_status pl_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	plrt *r = RT(rt) ;
	const char *p ;
	for (p = name ; *p ; p++){
		if (! (isALNUM(*p)||(*p == ':'))){
			return fail(ctx, r, UGLY_ERR_USAGE, "Error", "not a module name") ;
		}
	}
	ENTER_PERL(r) ;
	ENTER ;
	SAVETMPS ;
	SV *code = sv_2mortal(newSVpvf("require %s ; 1", name)) ;
	eval_sv(code, G_SCALAR | G_DISCARD) ;
	ugly_status st ;
	if (SvTRUE(ERRSV)){
		st = perl_failure(aTHX_ ctx, r) ;
	}
	else {
		st = to_val(aTHX_ ctx, r, sv_2mortal(new_package(aTHX_ name)), UGLY_HANDLE, out) ;
	}
	FREETMPS ;
	LEAVE ;
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	ugly_status st = to_val(aTHX_ ctx, r, sv_2mortal(new_package(aTHX_ "main")), UGLY_HANDLE, out) ;
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_eval(ugly_ctx *ctx, ugly_rt *rt, const char *code, size_t len, ugly_kind want, ugly_val *out){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	ENTER ;
	SAVETMPS ;
	/* No PUSHMARK: eval_sv pushes its own (an extra mark corrupts the caller's stack frame). */
	int count = eval_sv(sv_2mortal(newSVpvn(code, len)), G_LIST) ;
	dSP ;
	ugly_status st ;
	if (SvTRUE(ERRSV)){
		SP -= count ;
		PUTBACK ;
		st = perl_failure(aTHX_ ctx, r) ;
	}
	else {
		SV *result ;
		if (count == 0){
			result = sv_2mortal(newSV(0)) ;
		}
		else if (count == 1){
			result = POPs ;
		}
		else {
			AV *av = newAV() ;
			int k ;
			for (k = count - 1 ; k >= 0 ; k--){
				av_store(av, k, newSVsv(POPs)) ;
			}
			result = sv_2mortal(newRV_noinc((SV *)av)) ;
		}
		PUTBACK ;
		st = to_val(aTHX_ ctx, r, result, want, out) ;
	}
	FREETMPS ;
	LEAVE ;
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	uint32_t t = 0 ;
	const char *name = "SCALAR" ;
	char buf[256] ;
	if (IS_PACKAGE(sv)){
		const char *pkg = PACKAGE_NAME(sv) ;
		t = UGLY_TRAIT_MEMBERS ;
		name = "module" ;
		HV *stash = gv_stashpv(pkg, 0) ;
		if ((stash != NULL)&&(gv_fetchmethod_autoload(stash, "new", FALSE) != NULL)){
			t |= UGLY_TRAIT_INSTANTIABLE ;
			name = "class" ;
		}
	}
	else if (SvROK(sv)){
		SV *target = SvRV(sv) ;
		switch (SvTYPE(target)){
			case SVt_PVAV: t = UGLY_TRAIT_ELEMENTS ; name = "ARRAY" ; break ;
			case SVt_PVHV: t = UGLY_TRAIT_MEMBERS ; name = "HASH" ; break ;
			case SVt_PVCV: t = UGLY_TRAIT_CALLABLE ; name = "CODE" ; break ;
			default: break ;
		}
		if (SvOBJECT(target)){
			t |= UGLY_TRAIT_MEMBERS ;     /* methods */
			snprintf(buf, sizeof(buf), "%s", HvNAME(SvSTASH(target))) ;
			name = buf ;
		}
	}
	*traits = t ;
	ugly_status st = ugly_val_set_string(ctx, type_name, name, strlen(name)) ;
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st = UGLY_ERR_NOT_FOUND ;
	if (IS_PACKAGE(sv)){
		/* Members of a package: nested packages, then subs. */
		const char *pkg = PACKAGE_NAME(sv) ;
		char *nested = nested_package(aTHX_ pkg, name) ;
		if (nested != NULL){
			st = to_val(aTHX_ ctx, r, sv_2mortal(new_package(aTHX_ nested)), want, out) ;
			free(nested) ;
		}
		else {
			CV *cv = package_sub(aTHX_ pkg, name) ;
			if (cv != NULL){
				st = to_val(aTHX_ ctx, r, sv_2mortal(newRV_inc((SV *)cv)), want, out) ;
			}
		}
	}
	else if (SvROK(sv)){
		SV *target = SvRV(sv) ;
		/* A hash's entries, then an object's methods. */
		if (SvTYPE(target) == SVt_PVHV){
			SV **v = hv_fetch((HV *)target, name, (I32)strlen(name), 0) ;
			if (v != NULL){
				st = to_val(aTHX_ ctx, r, *v, want, out) ;
			}
		}
		if ((st == UGLY_ERR_NOT_FOUND)&&SvOBJECT(target)){
			GV *gv = gv_fetchmethod_autoload(SvSTASH(target), name, FALSE) ;
			if ((gv != NULL)&&isGV(gv)&&(GvCV(gv) != NULL)){
				st = to_val(aTHX_ ctx, r, sv_2mortal(newRV_inc((SV *)GvCV(gv))), want, out) ;
			}
		}
		if ((st == UGLY_ERR_NOT_FOUND)&&(SvTYPE(target) != SVt_PVHV)&&(! SvOBJECT(target))){
			st = UGLY_ERR_NOT_CAPABLE ;
		}
	}
	else {
		st = UGLY_ERR_NOT_CAPABLE ;
	}
	if (st == UGLY_ERR_NOT_FOUND){
		st = fail(ctx, r, st, "Error", name) ;
	}
	else if (st == UGLY_ERR_NOT_CAPABLE){
		st = fail(ctx, r, st, "TypeError", "value has no members") ;
	}
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st = UGLY_OK ;
	if ((! SvROK(sv))||IS_PACKAGE(sv)||(SvTYPE(SvRV(sv)) != SVt_PVHV)){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "only hashes take members") ;
	}
	else {
		SV *pv = from_val(aTHX_ ctx, r, v, &st) ;
		if (pv != NULL){
			(void)hv_store((HV *)SvRV(sv), name, (I32)strlen(name), pv, 0) ;
		}
	}
	LEAVE_PERL() ;
	return st ;
}


/* delete $h->{name}; a package's members (subs) aren't deleted. */
static ugly_status pl_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st = UGLY_OK ;
	if ((! SvROK(sv))||IS_PACKAGE(sv)||(SvTYPE(SvRV(sv)) != SVt_PVHV)){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "only hashes have members to delete") ;
	}
	else {
		(void)hv_delete((HV *)SvRV(sv), name, (I32)strlen(name), G_DISCARD) ;
	}
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	HV *hv = NULL ;
	int package = IS_PACKAGE(sv) ;
	if (package){
		hv = gv_stashpv(PACKAGE_NAME(sv), 0) ;
	}
	else if (SvROK(sv)&&(SvTYPE(SvRV(sv)) == SVt_PVHV)){
		hv = (HV *)SvRV(sv) ;
	}
	ugly_status st ;
	if (hv == NULL){
		st = (package ? ugly_list_new(ctx, NULL, 0, out) : fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no members")) ;
	}
	else {
		size_t cap = 16, n = 0 ;
		ugly_val *names = malloc(cap * sizeof(ugly_val)) ;
		char **keep = malloc(cap * sizeof(char *)) ;
		HE *he ;
		hv_iterinit(hv) ;
		while ((he = hv_iternext(hv)) != NULL){
			I32 klen ;
			const char *k = hv_iterkey(he, &klen) ;
			char *copy ;
			if (package){
				/* A package's subs, and its nested packages (Name::). */
				SV *val = hv_iterval(hv, he) ;
				int nested = ((klen > 2)&&(strcmp(k + klen - 2, "::") == 0)) ;
				if (! nested && ! (isGV(val)&&GvCV((GV *)val))){
					continue ;
				}
				copy = strndup(k, (size_t)(nested ? klen - 2 : klen)) ;
			}
			else {
				copy = strndup(k, (size_t)klen) ;
			}
			if (n == cap){
				cap *= 2 ;
				names = realloc(names, cap * sizeof(ugly_val)) ;
				keep = realloc(keep, cap * sizeof(char *)) ;
			}
			keep[n] = copy ;
			names[n] = ugly_str(copy, strlen(copy)) ;
			n++ ;
		}
		st = ugly_list_new(ctx, names, n, out) ;
		size_t i ;
		for (i = 0 ; i < n ; i++){
			free(keep[i]) ;
		}
		free(keep) ;
		free(names) ;
	}
	LEAVE_PERL() ;
	return st ;
}


static AV *array_of(pTHX_ SV *sv){
	return ((SvROK(sv)&&(! IS_PACKAGE(sv))&&(SvTYPE(SvRV(sv)) == SVt_PVAV)) ? (AV *)SvRV(sv) : NULL) ;
}


static ugly_status pl_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	AV *av = array_of(aTHX_ ref) ;
	ugly_status st = UGLY_OK ;
	if (av == NULL){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
	}
	else {
		*out = (int64_t)(av_len(av) + 1) ;
	}
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	AV *av = array_of(aTHX_ ref) ;
	ugly_status st ;
	if (av == NULL){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
	}
	else if ((i < 0)||(i > (int64_t)av_len(av))){
		st = fail(ctx, r, UGLY_ERR_NOT_FOUND, "Error", "index out of range") ;
	}
	else {
		SV **v = av_fetch(av, (SSize_t)i, 0) ;
		st = to_val(aTHX_ ctx, r, (v != NULL ? *v : &PL_sv_undef), want, out) ;
	}
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	AV *av = array_of(aTHX_ ref) ;
	ugly_status st = UGLY_OK ;
	if (av == NULL){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
	}
	else if ((i < 0)||(i > (int64_t)av_len(av) + 1)){
		st = fail(ctx, r, UGLY_ERR_NOT_FOUND, "Error", "index out of range") ;
	}
	else {
		SV *pv = from_val(aTHX_ ctx, r, v, &st) ;     /* one past the end appends */
		if (pv != NULL){
			av_store(av, (SSize_t)i, pv) ;
		}
	}
	LEAVE_PERL() ;
	return st ;
}


/* splice(@a, offset, remove, @insert): the tail is kept, the array cut at offset, then refilled. */
static ugly_status pl_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){

	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	AV *av = array_of(aTHX_ ref) ;
	ugly_status st = UGLY_OK ;
	if (av == NULL){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
		LEAVE_PERL() ;
		return st ;
	}
	SSize_t len = av_len(av) + 1, off = (SSize_t)offset, tail = len - off - (SSize_t)remove, j ;
	SV **ins = calloc(n + 1, sizeof(SV *)) ;
	SV **keep = calloc((size_t)tail + 1, sizeof(SV *)) ;
	size_t i ;
	for (i = 0 ; i < n ; i++){
		if ((ins[i] = from_val(aTHX_ ctx, r, insert[i], &st)) == NULL){
			while (i-- > 0){
				SvREFCNT_dec(ins[i]) ;
			}
			break ;
		}
	}
	if (st == UGLY_OK){
		for (j = 0 ; j < tail ; j++){
			SV **e = av_fetch(av, off + (SSize_t)remove + j, 0) ;
			keep[j] = ((e != NULL) ? SvREFCNT_inc(*e) : newSV(0)) ;
		}
		av_fill(av, off - 1) ;     /* frees the removed elements (and our tail's slots) */
		for (i = 0 ; i < n ; i++){
			av_push(av, ins[i]) ;
		}
		for (j = 0 ; j < tail ; j++){
			av_push(av, keep[j]) ;
		}
	}
	free(ins) ;
	free(keep) ;
	LEAVE_PERL() ;
	return st ;
}


static ugly_status call_entry(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_kind want, ugly_val *out){

	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st ;
	if ((! SvROK(sv))||(SvTYPE(SvRV(sv)) != SVt_PVCV)){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not code") ;
	}
	else {
		st = call_perl(aTHX_ ctx, r, sv, NULL, NULL, args, nargs, o, want, out) ;
	}
	LEAVE_PERL() ;
	return st ;
}


/* A package's function is called as pkg::f(args); an object's method as $obj->m(args); a hash's code entry as is. */
static ugly_status invoke_entry(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *o, ugly_kind want, ugly_val *out){

	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st ;
	if (IS_PACKAGE(sv)){
		CV *cv = package_sub(aTHX_ PACKAGE_NAME(sv), method) ;
		st = (cv == NULL ? fail(ctx, r, UGLY_ERR_NOT_FOUND, "Error", method)
			: call_perl(aTHX_ ctx, r, sv_2mortal(newRV_inc((SV *)cv)), NULL, NULL, args, nargs, o, want, out)) ;
	}
	else if (SvROK(sv)&&SvOBJECT(SvRV(sv))){
		if (gv_fetchmethod_autoload(SvSTASH(SvRV(sv)), method, TRUE) == NULL){
			st = fail(ctx, r, UGLY_ERR_NOT_FOUND, "Error", method) ;
		}
		else {
			st = call_perl(aTHX_ ctx, r, NULL, method, sv, args, nargs, o, want, out) ;
		}
	}
	else if (SvROK(sv)&&(SvTYPE(SvRV(sv)) == SVt_PVHV)){
		SV **v = hv_fetch((HV *)SvRV(sv), method, (I32)strlen(method), 0) ;
		st = (((v == NULL)||(! SvROK(*v))||(SvTYPE(SvRV(*v)) != SVt_PVCV)) ? fail(ctx, r, UGLY_ERR_NOT_FOUND, "Error", method)
			: call_perl(aTHX_ ctx, r, *v, NULL, NULL, args, nargs, o, want, out)) ;
	}
	else {
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no methods") ;
	}
	LEAVE_PERL() ;
	return st ;
}


static ugly_status new_entry(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_handle *out){

	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st ;
	if (! IS_PACKAGE(sv)){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "only packages are instantiable") ;
	}
	else {
		ugly_val v = ugly_null() ;
		SV *cls = sv_2mortal(newSVpv(PACKAGE_NAME(sv), 0)) ;
		st = call_perl(aTHX_ ctx, r, NULL, "new", cls, args, nargs, o, UGLY_HANDLE, &v) ;
		if (st == UGLY_OK){
			*out = v.as.h ;
		}
	}
	LEAVE_PERL() ;
	return st ;
}


static ugly_status pl_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){
	return call_entry(ctx, rt, ref, args, nargs, NULL, want, out) ;
}

static ugly_status pl_call_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_kind want, ugly_val *out){
	return call_entry(ctx, rt, ref, args, nargs, o, want, out) ;
}

static ugly_status pl_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){
	return invoke_entry(ctx, rt, ref, method, args, nargs, NULL, want, out) ;
}

static ugly_status pl_invoke_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *o, ugly_kind want, ugly_val *out){
	return invoke_entry(ctx, rt, ref, method, args, nargs, o, want, out) ;
}

static ugly_status pl_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){
	return new_entry(ctx, rt, ref, args, nargs, NULL, out) ;
}

static ugly_status pl_new_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_handle *out){
	return new_entry(ctx, rt, ref, args, nargs, o, out) ;
}


static void pl_release(ugly_rt *rt, void *ref){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SvREFCNT_dec((SV *)ref) ;     /* may run DESTROY: this interpreter must be current */
	LEAVE_PERL() ;
}


/* ---------------------------------------------------------------- */
/* Lifecycle                                                        */
/* ---------------------------------------------------------------- */

extern char **environ ;

/*
 * perl_construct ignores SIGFPE (PERL_FPU_INIT). Another runtime in the
 * process may own that signal: the JVM turns it into ArithmeticException.
 * So we keep whatever handler was there before perl started.
 */
static void keep_sigfpe(struct sigaction *saved, int restore){
	if (! restore){
		sigaction(SIGFPE, NULL, saved) ;
	}
	else if ((saved->sa_flags & SA_SIGINFO)||(saved->sa_handler != SIG_DFL)){
		sigaction(SIGFPE, saved, NULL) ;
	}
}


static ugly_status pl_init(ugly_ctx *ctx, const char *dir){
	(void)ctx ; (void)dir ;
	/* Once per process, and only if we're the first perl (PL_curinterp stays set after perl_free). */
	if (PL_curinterp == NULL){
		static char *args[] = { "", NULL } ;
		int argc = 1 ;
		char **argv = args ;
		char **env = environ ;
		struct sigaction fpe ;
		keep_sigfpe(&fpe, 0) ;
		PERL_SYS_INIT3(&argc, &argv, &env) ;
		keep_sigfpe(&fpe, 1) ;
	}
	return UGLY_OK ;
}


static void add_runtime(plrt *r){
	r->next = runtimes ;
	runtimes = r ;
}


static void remove_runtime(plrt *r){
	plrt **p ;
	for (p = &runtimes ; *p != NULL ; p = &(*p)->next){
		if (*p == r){
			*p = r->next ;
			return ;
		}
	}
}


static ugly_status pl_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	plrt *r = calloc(1, sizeof(plrt)) ;
	if (r == NULL){
		return UGLY_ERR_NOMEM ;
	}
	r->urt = rt ;
	r->owns = 1 ;
	PerlInterpreter *saved_perl_ = PERL_GET_CONTEXT ;
	struct sigaction fpe ;
	keep_sigfpe(&fpe, 0) ;
	PerlInterpreter *my_perl = perl_alloc() ;     /* makes the new interpreter current */
	r->perl = my_perl ;
	perl_construct(my_perl) ;
	PL_exit_flags |= PERL_EXIT_DESTRUCT_END ;
	static char *embedding[] = { "", "-e", "0", NULL } ;
	ugly_status st = UGLY_OK ;
	if ((perl_parse(my_perl, xs_init, 3, embedding, environ) != 0)||(perl_run(my_perl) != 0)||(load_helper(aTHX) != 0)){
		st = ugly_fail(ctx, rt, UGLY_ERR_INTERNAL, "Error", "can't start perl") ;
		perl_destruct(my_perl) ;
		perl_free(my_perl) ;
		free(r) ;
	}
	else {
		/* ugly_configure("perl5", "path", dir): ahead of PERL5LIB in @INC, in the order given. */
		AV *inc = GvAVn(PL_incgv) ;
		size_t n = 0 ;
		while (ugly_option("perl5", "path", n) != NULL){
			n++ ;
		}
		while (n-- > 0){
			av_unshift(inc, 1) ;
			av_store(inc, 0, newSVpv(ugly_option("perl5", "path", n), 0)) ;
		}
		add_runtime(r) ;
		*native = r ;
	}
	keep_sigfpe(&fpe, 1) ;
	PERL_SET_CONTEXT(saved_perl_) ;
	return st ;
}


/* The host's interpreter, when perl is the host (the binding). */
static ugly_status pl_rt_attach(ugly_ctx *ctx, ugly_rt *rt, void *existing, void **native){
	(void)ctx ;
	if (existing == NULL){
		return UGLY_ERR_USAGE ;
	}
	plrt *r = calloc(1, sizeof(plrt)) ;
	if (r == NULL){
		return UGLY_ERR_NOMEM ;
	}
	r->urt = rt ;
	r->owns = 0 ;
	r->perl = existing ;
	add_runtime(r) ;
	*native = r ;
	return UGLY_OK ;
}


static ugly_status pl_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ;
	plrt *r = native ;
	remove_runtime(r) ;
	if (r->owns){
		PerlInterpreter *saved = PERL_GET_CONTEXT ;
		PerlInterpreter *my_perl = r->perl ;
		PERL_SET_CONTEXT(my_perl) ;
		perl_destruct(my_perl) ;     /* END blocks and the remaining DESTROYs run here */
		perl_free(my_perl) ;
		PERL_SET_CONTEXT(saved == my_perl ? NULL : saved) ;
	}
	size_t i ;
	for (i = 0 ; i < sizeof(r->pcache) / sizeof(r->pcache[0]) ; i++){
		while (r->pcache[i] != NULL){
			pcache_entry *e = r->pcache[i] ;
			r->pcache[i] = e->next ;
			free(e) ;
		}
	}
	free(r) ;
	return UGLY_OK ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_MULTI_RUNTIME | UGLY_CAP_EVAL | UGLY_CAP_ATTACH,
	.name = "perl5",
	.init = pl_init,
	.rt_new = pl_rt_new,
	.options = (const char *const []){ "path", NULL },
	.rt_attach = pl_rt_attach,
	.rt_delete = pl_rt_delete,
	.import = pl_import,
	.globals = pl_globals,
	.eval = pl_eval,
	.describe = pl_describe,
	.get_member = pl_get_member,
	.set_member = pl_set_member,
	.member_names = pl_member_names,
	.length = pl_length,
	.get_index = pl_get_index,
	.set_index = pl_set_index,
	.call_opts = pl_call_opts,
	.invoke_opts = pl_invoke_opts,
	.new_opts = pl_new_opts,
	.delete_member = pl_delete_member,
	.splice = pl_splice,
	.call = pl_call,
	.invoke = pl_invoke,
	.new_instance = pl_new_instance,
	.release = pl_release,
} ;


const ugly_plugin *ugly_plugin_perl5(void){
	return &plugin ;
}


#ifdef UGLY_BINDING
/* ---------------------------------------------------------------- */
/* The binding: use Ugly from a plain perl program                  */
/* ---------------------------------------------------------------- */

/*
 * Built from this same file as the XS module auto/Ugly/Ugly.so, without
 * libperl: its perl symbols come from the host perl (statically linked,
 * exporting its API). Ugly.pm loads it with XSLoader. boot_Ugly defines the
 * helper packages, registers this plugin as "perl5" and attaches to the
 * running interpreter. Ugly.pm's END block tears down, before global
 * destruction.
 */
static ugly_ctx *b_ctx ;
static ugly_lang *b_lang ;
static ugly_rt *b_rt ;
static int b_owns_ctx ;

XS(xs_teardown){
	dXSARGS ;
	(void)items ;
	if (b_rt != NULL){
		ugly_rt_delete(b_ctx, b_rt) ;
		ugly_lang_unload(b_ctx, b_lang) ;
		if (b_owns_ctx){
			ugly_ctx_delete(b_ctx) ;     /* also deletes the runtimes Ugly::module created */
		}
		b_rt = NULL ;
	}
	XSRETURN_EMPTY ;
}


XS_EXTERNAL(boot_Ugly) ;
XS_EXTERNAL(boot_Ugly){
	dXSARGS ;
	(void)items ;
	if (b_rt == NULL){
		register_xs(aTHX) ;
		newXS("Ugly::_teardown", xs_teardown, __FILE__) ;
		if (load_helper(aTHX) != 0){
			croak("ugly: can't define the helper packages: %s", SvPV_nolen(ERRSV)) ;
		}
		b_ctx = ugly_current_ctx() ;
		if (b_ctx == NULL){
			if (ugly_ctx_new(&b_ctx) != UGLY_OK){
				croak("ugly: can't create a context") ;
			}
			b_owns_ctx = 1 ;
		}
		ugly_status st = ugly_lang_register(b_ctx, &plugin, &b_lang) ;
		if (st == UGLY_OK){
			st = ugly_rt_attach(b_ctx, b_lang, my_perl, &b_rt) ;
		}
		if (st != UGLY_OK){
			const ugly_error *e = ugly_last_error(b_ctx) ;
			croak("ugly: can't attach: %s", e ? ugly_error_message(e) : "?") ;
		}
	}
	XSRETURN_YES ;
}
#endif
