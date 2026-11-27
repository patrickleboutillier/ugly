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
 *
 * Iteration (contract §2, decision 21): arrays, and iterators, which are
 * objects with a next method returning the empty list at the end.
 * Ugly::iter($x) gives one over anything iterable, foreign values too.
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
	struct guard *guard ;        /* the entry running under eval (protect), innermost */
	CV *guard_cv ;               /* Ugly::_guard, which runs it */
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


/*
 * Every entry into an interpreter: make it current, and restore the caller's
 * on the way out. Each is a scope for temporaries too: a caller that isn't
 * Perl has none around us, and the mortals an entry makes (the RV it calls,
 * an iterator) would otherwise live until the interpreter is destroyed.
 */
#define ENTER_PERL(r) PerlInterpreter *saved_perl_ = PERL_GET_CONTEXT ; PerlInterpreter *my_perl = (r)->perl ; \
	PERL_SET_CONTEXT(my_perl) ; ENTER ; SAVETMPS
#define LEAVE_PERL() FREETMPS ; LEAVE ; PERL_SET_CONTEXT(saved_perl_)


/* $@ after G_EVAL: an exception object is an error whatever its bool (or "") overload says, or does. */
#define DIED (SvROK(ERRSV)||SvTRUE(ERRSV))


/*
 * A hash key's length for hv_fetch and friends, from a name the core gave
 * us (UTF-8): negative says "this key is UTF-8", which a name with
 * characters beyond ASCII must say to find the key Perl code stored.
 */
static I32 key_len(const char *name){
	size_t n = strlen(name), i ;
	for (i = 0 ; i < n ; i++){
		if ((unsigned char)name[i] >= 0x80){
			return -(I32)n ;
		}
	}
	return (I32)n ;
}


/*
 * The same for a member of a hash. Perl programs often carry undecoded
 * UTF-8 (decision 6), keys included: when the hash has no such character
 * key but has the name's bytes as a key, that is the member.
 */
static I32 member_len(pTHX_ HV *hv, const char *name){
	I32 n = key_len(name) ;
	if ((n < 0)&&(! hv_exists(hv, name, n))&&hv_exists(hv, name, -n)){
		return -n ;
	}
	return n ;
}


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


/*
 * An iterator (contract §2): an object with a next method, which returns the
 * next value, or the empty list at the end (and again after). Perl has no
 * iteration protocol; this is the shape of Ugly::iter's iterators, and of
 * most hand-written ones.
 */
static int is_iterator(pTHX_ SV *sv){
	if ((! SvROK(sv))||(! SvOBJECT(SvRV(sv)))||IS_PACKAGE(sv)||IS_PROXY(sv)){
		return 0 ;
	}
	return gv_fetchmethod_autoload(SvSTASH(SvRV(sv)), "next", FALSE) != NULL ;
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


/*
 * A value's string, for an error message: a temporary's text, good until
 * the caller's temporaries go. An object's goes through Perl under eval,
 * since an overloaded "" may die; then it is its class's name.
 */
static const char *message_of(pTHX_ SV *sv){
	SV *text ;
	if (! (SvROK(sv)&&SvOBJECT(SvRV(sv)))){
		/*
		 * A copy of its characters, made by hand: sv_mortalcopy of a
		 * temporary takes its buffer away (perl's "steal" optimization),
		 * and sv is the caller's copy of $@, the payload to be.
		 */
		STRLEN n ;
		const char *chars = SvPV(sv, n) ;
		text = newSVpvn_flags(chars, n, SVs_TEMP | (SvUTF8(sv) ? SVf_UTF8 : 0)) ;
	}
	else {
		ENTER ;
		SAVETMPS ;
		dSP ;
		PUSHMARK(SP) ;
		XPUSHs(sv) ;
		PUTBACK ;
		int count = call_pv("Ugly::_str", G_SCALAR | G_EVAL) ;
		SPAGAIN ;
		if (DIED){
			SP -= count ;
			sv_setsv(ERRSV, &PL_sv_undef) ;
			text = newSVpv(HvNAME(SvSTASH(SvRV(sv))), 0) ;
		}
		else {
			text = newSVsv(POPs) ;
		}
		PUTBACK ;
		FREETMPS ;
		LEAVE ;
		sv_2mortal(text) ;     /* the caller's temporary: it outlives the scope its value was made in */
	}
	/* Text: a byte string that isn't UTF-8 already is Latin-1, as perl itself reads it. */
	STRLEN len ;
	const char *p = SvPV(text, len) ;
	if ((! SvUTF8(text))&&(! valid_utf8((const unsigned char *)p, len))){
		p = SvPVutf8_nolen(text) ;     /* a variable: the macro evaluates its argument twice */
	}
	return p ;
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
	char type[128] = "die" ;
	const char *message = NULL ;
	if (SvROK(e)&&SvOBJECT(SvRV(e))){
		snprintf(type, sizeof(type), "%s", HvNAME(SvSTASH(SvRV(e)))) ;
		if (SvTYPE(SvRV(e)) == SVt_PVHV){
			SV **m = hv_fetchs((HV *)SvRV(e), "message", 0) ;
			if ((m != NULL)&&(SvOK(*m))){
				message = message_of(aTHX_ *m) ;
			}
		}
	}
	if ((message == NULL)||(message[0] == '\0')){
		message = message_of(aTHX_ e) ;     /* whole: a temporary's text, which the core copies */
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
static ugly_status to_val_guarded(pTHX_ ugly_ctx *ctx, plrt *r, SV *sv, ugly_kind want, ugly_val *out) ;

/*
 * Whether using a value can run Perl code, which may die: it's tied, or
 * (when a primitive is wanted of it) its class overloads the conversions.
 */
static int runs_code(SV *sv, int converting){
	return SvMAGICAL(sv)||(SvROK(sv)&&(SvMAGICAL(SvRV(sv))||(converting && SvAMAGIC(sv)))) ;
}

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
	if (DIED){
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
		/* The call ran under eval; converting its result doesn't, unless it has to. */
		st = (runs_code(result, (want != UGLY_ANY)&&(want != UGLY_HANDLE)) ? to_val_guarded(aTHX_ ctx, r, result, want, out)
			: to_val(aTHX_ ctx, r, result, want, out)) ;
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


/* Ugly::Core::_call($proxy, @args) */
/*
 * Keyword arguments (contract §11): Ugly::kw(name => value, ...) as the last
 * argument, an Ugly::Kw object holding the pairs in order. If it's there,
 * *n shrinks by one and k holds them (names point into the object, which
 * is on the stack; values are owned: free_kw).
 */
typedef struct { ugly_opts o ; ugly_kwarg *kw ; } perl_kw ;

static void free_kw(ugly_ctx *ctx, perl_kw *k) ;

static int is_marker(pTHX_ SV *sv, const char *cls){
	return sv_isobject(sv)&&sv_derived_from(sv, cls) ;
}


/* Ugly::opts(hint => ..., context => ...): its strings stay in the object, on the stack for the call. */
static void take_opts(pTHX_ SV *opts, ugly_opts *o){
	HV *hv = (HV *)SvRV(opts) ;
	SV **hint = hv_fetchs(hv, "hint", 0), **context = hv_fetchs(hv, "context", 0) ;
	o->hint = ((hint != NULL)&&SvOK(*hint) ? SvPVutf8_nolen(*hint) : NULL) ;
	o->context = ((context != NULL)&&SvOK(*context) ? SvPVutf8_nolen(*context) : NULL) ;
}


static void take_kw(pTHX_ ugly_ctx *ctx, plrt *r, SV **args, int *n, perl_kw *k){
	k->o = (ugly_opts)UGLY_OPTS_INIT ;
	k->kw = NULL ;
	/* The trailing Ugly::kw(...) and Ugly::opts(...), at most one of each, in either order. */
	SV *kwsv = NULL ;
	int have_opts = 0 ;
	while (*n > 0){
		SV *last = args[*n - 1] ;
		if ((! have_opts)&&is_marker(aTHX_ last, "Ugly::Opts")){
			take_opts(aTHX_ last, &k->o) ;
			have_opts = 1 ;
		}
		else if ((kwsv == NULL)&&is_marker(aTHX_ last, "Ugly::Kw")){
			kwsv = last ;
		}
		else {
			break ;
		}
		(*n)-- ;
	}
	if (kwsv == NULL){
		return ;
	}
	AV *av = (AV *)SvRV(kwsv) ;
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


/* Ugly::Core::_invoke($proxy, $method, @args) */
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


/* Ugly::Core::_get($proxy, $key): an element for an integer key on an element collection, else a member. */
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


/* Ugly::Core::_set($proxy, $key, $value, $is_index) */
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


/*
 * Entries (contract §2), behind %$proxy for a foreign keyed collection and
 * behind Ugly::entry and friends. The key is any Perl value.
 *
 * Ugly::Core::_entry_get($proxy, $key): the value, or the empty list if it isn't there.
 */
XS(xs_entry_get){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 2)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_val key, out = ugly_null() ;
	ugly_status st = to_val(aTHX_ ctx, r, ST(1), UGLY_ANY, &key) ;
	if (st == UGLY_OK){
		st = ugly_get_entry(ctx, PROXY_HANDLE(ST(0)), key, UGLY_ANY, &out) ;
		ugly_val_release(ctx, &key) ;
	}
	if (st == UGLY_ERR_NOT_FOUND){
		XSRETURN_EMPTY ;
	}
	ST(0) = xs_result(aTHX_ ctx, r, st, &out) ;
	XSRETURN(1) ;
}


/* Ugly::Core::_entry_set($proxy, $key, $value) */
XS(xs_entry_set){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 3)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_val key, v = ugly_null() ;
	ugly_status st = to_val(aTHX_ ctx, r, ST(1), UGLY_ANY, &key) ;
	if (st == UGLY_OK){
		if ((st = to_val(aTHX_ ctx, r, ST(2), UGLY_ANY, &v)) == UGLY_OK){
			st = ugly_set_entry(ctx, PROXY_HANDLE(ST(0)), key, v) ;
			ugly_val_release(ctx, &v) ;
		}
		ugly_val_release(ctx, &key) ;
	}
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	XSRETURN_EMPTY ;
}


/* Ugly::Core::_entry_delete($proxy, $key) */
XS(xs_entry_delete){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 2)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_val key ;
	ugly_status st = to_val(aTHX_ ctx, r, ST(1), UGLY_ANY, &key) ;
	if (st == UGLY_OK){
		st = ugly_delete_entry(ctx, PROXY_HANDLE(ST(0)), key) ;
		ugly_val_release(ctx, &key) ;
	}
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	XSRETURN_EMPTY ;
}


/* Ugly::Core::_entry_keys($proxy): the keys, as they are now. */
XS(xs_entry_keys){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 1)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_handle it = UGLY_NO_HANDLE ;
	ugly_status st = ugly_entry_keys(ctx, PROXY_HANDLE(ST(0)), &it) ;
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	/* The keys first, then the stack: a call through the core can move perl's stack (see xs_proxy_keys). */
	AV *keys = (AV *)sv_2mortal((SV *)newAV()) ;
	for (;;){
		ugly_val k = ugly_null() ;
		size_t got = 0 ;
		st = ugly_next(ctx, it, UGLY_ANY, &k, 1, &got) ;
		if ((st != UGLY_OK)||(got == 0)){
			break ;
		}
		SV *sv = from_val(aTHX_ ctx, r, k, &st) ;
		ugly_val_release(ctx, &k) ;
		if (sv == NULL){
			break ;
		}
		av_push(keys, sv) ;
	}
	ugly_release(ctx, it) ;
	if (st != UGLY_OK){
		croak_ugly(aTHX_ ctx, r, st) ;
	}
	SSize_t n = av_len(keys) + 1, i ;
	SPAGAIN ;
	SP -= items ;
	EXTEND(SP, n) ;
	for (i = 0 ; i < n ; i++){
		PUSHs(sv_mortalcopy(*av_fetch(keys, i, 0))) ;
	}
	PUTBACK ;
}


/* Ugly::Core::_has($proxy, $name): whether the member exists (even if it is undef). */
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


/* Ugly::Core::_delete($proxy, $name) */
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


/* Ugly::Core::_splice($proxy, $offset, $remove, @insert) */
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


/* Ugly::Core::_new($class_proxy, @args) */
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


/* Ugly::Core::_length($proxy) */
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


/* Ugly::Core::_keys($proxy): the member names. */
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
	/*
	 * The names first, then the stack: a call through the core can come back
	 * into this interpreter (a Python __dir__ calling a Perl sub) and move
	 * perl's stack, which leaves our copy of its pointer behind.
	 */
	int64_t n = 0, i, got = 0 ;
	ugly_length(ctx, names, &n) ;
	SV **keys = calloc((size_t)(n > 0 ? n : 0) + 1, sizeof(SV *)) ;
	for (i = 0 ; (keys != NULL)&&(i < n) ; i++){
		ugly_val s ;
		if (ugly_get_index(ctx, names, i, UGLY_STRING, &s) == UGLY_OK){
			keys[got] = sv_2mortal(newSVpvn(s.as.str.ptr, s.as.str.len)) ;
			SvUTF8_on(keys[got++]) ;
			ugly_val_release(ctx, &s) ;
		}
	}
	ugly_release(ctx, names) ;
	SPAGAIN ;
	SP -= items ;
	EXTEND(SP, got) ;
	for (i = 0 ; i < got ; i++){
		PUSHs(keys[i]) ;
	}
	free(keys) ;
	PUTBACK ;
}


/* Ugly::Core::_traits($proxy) */
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


/* Ugly::Core::_same($a, $b) */
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


/* Ugly::Core::_release($proxy): from DESTROY. */
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


/*
 * Ugly::Core::_held($proxy): another proxy for the same value, owning its
 * own reference and outside the cache. The code ref &{$proxy} gives closes
 * over it, so it can be cached without keeping $proxy alive, and outlive it.
 */
XS(xs_proxy_held){
	dXSARGS ;
	if ((items < 1)||(! IS_PROXY(ST(0)))){
		croak("Ugly::Core::_held: not a proxy") ;
	}
	ugly_ctx *ctx = ugly_current_ctx() ;
	ugly_handle h ;
	if ((ctx == NULL)||(ugly_dup(ctx, PROXY_HANDLE(ST(0)), &h) != UGLY_OK)){
		croak("Ugly::Core::_held: stale proxy") ;
	}
	SV *rv = newRV_noinc(newSVuv((UV)h)) ;
	sv_bless(rv, gv_stashpv("Ugly::Proxy", GV_ADD)) ;
	ST(0) = sv_2mortal(rv) ;
	XSRETURN(1) ;
}


/*
 * Ugly::Core::_iter($proxy): an iterator over a foreign value, as a proxy;
 * undef if its language doesn't iterate it (Ugly::iter then walks its
 * elements, if it has any).
 */
XS(xs_proxy_iter){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 1)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_handle it = UGLY_NO_HANDLE ;
	ugly_status st = ugly_iter(ctx, PROXY_HANDLE(ST(0)), &it) ;
	if ((st == UGLY_ERR_NOT_CAPABLE)||(st == UGLY_ERR_NOT_IMPLEMENTED)){
		XSRETURN_UNDEF ;
	}
	ugly_val v = ugly_hval(it) ;
	ST(0) = xs_result(aTHX_ ctx, r, st, &v) ;
	XSRETURN(1) ;
}


/* Ugly::Core::_next($iterator): its next value, or the empty list at the end. One at a time: never ahead. */
XS(xs_proxy_next){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = xs_ctx(aTHX) ;
	if ((items < 1)||(! IS_PROXY(ST(0)))){
		croak("ugly: not a proxy") ;
	}
	ugly_val v = ugly_null() ;
	size_t got = 0 ;
	ugly_status st = ugly_next(ctx, PROXY_HANDLE(ST(0)), UGLY_ANY, &v, 1, &got) ;
	if ((st == UGLY_OK)&&(got == 0)){
		XSRETURN_EMPTY ;
	}
	ST(0) = xs_result(aTHX_ ctx, r, st, &v) ;
	XSRETURN(1) ;
}


/*
 * Ugly::Core::_str($proxy): its language's stringification (ugly_to_string);
 * undef if it has none, or if no ctx is left (global destruction), so that
 * "" falls back to Ugly::Proxy(<handle>). An exception dies.
 */
XS(xs_proxy_str){
	dXSARGS ;
	plrt *r = R(my_perl) ;
	ugly_ctx *ctx = ugly_current_ctx() ;
	if ((items < 1)||(! IS_PROXY(ST(0)))||(ctx == NULL)||(r == NULL)){
		XSRETURN_UNDEF ;
	}
	ugly_val v = ugly_null() ;
	ugly_status st = ugly_to_string(ctx, PROXY_HANDLE(ST(0)), &v) ;
	if ((st == UGLY_ERR_NOT_SUPPORTED)||(st == UGLY_ERR_NOT_IMPLEMENTED)||(st == UGLY_ERR_NOT_CAPABLE)||(st == UGLY_ERR_STALE_HANDLE)){
		XSRETURN_UNDEF ;
	}
	ST(0) = xs_result(aTHX_ ctx, r, st, &v) ;
	XSRETURN(1) ;
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

static XS(xs_guard) ;     /* below, with the entries it runs */

static void register_xs(pTHX){
	const char *file = __FILE__ ;
	newXS("Ugly::Core::_call", xs_proxy_call, file) ;
	newXS("Ugly::Core::_invoke", xs_proxy_invoke, file) ;
	newXS("Ugly::Core::_get", xs_proxy_get, file) ;
	newXS("Ugly::Core::_set", xs_proxy_set, file) ;
	newXS("Ugly::Core::_length", xs_proxy_length, file) ;
	newXS("Ugly::Core::_has", xs_proxy_has, file) ;
	newXS("Ugly::Core::_delete", xs_proxy_delete, file) ;
	newXS("Ugly::Core::_splice", xs_proxy_splice, file) ;
	newXS("Ugly::Core::_new", xs_proxy_new, file) ;
	newXS("Ugly::Core::_keys", xs_proxy_keys, file) ;
	newXS("Ugly::Core::_traits", xs_proxy_traits, file) ;
	newXS("Ugly::Core::_same", xs_proxy_same, file) ;
	newXS("Ugly::Core::_release", xs_proxy_release, file) ;
	newXS("Ugly::Core::_held", xs_proxy_held, file) ;
	newXS("Ugly::Core::_iter", xs_proxy_iter, file) ;
	newXS("Ugly::Core::_next", xs_proxy_next, file) ;
	newXS("Ugly::Core::_str", xs_proxy_str, file) ;
	newXS("Ugly::Core::_entry_get", xs_entry_get, file) ;
	newXS("Ugly::Core::_entry_set", xs_entry_set, file) ;
	newXS("Ugly::Core::_entry_delete", xs_entry_delete, file) ;
	newXS("Ugly::Core::_entry_keys", xs_entry_keys, file) ;
	newXS("Ugly::Error::_free", xs_error_free, file) ;
	newXS("Ugly::module", xs_module, file) ;
	newXS("Ugly::eval", xs_eval, file) ;
	newXS("Ugly::_configure", xs_configure, file) ;
	newXS("Ugly::_guard", xs_guard, file) ;
}


static void xs_init(pTHX){
	newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, __FILE__) ;
	register_xs(aTHX) ;
}


/* Defines the helper packages (once per interpreter). 0 on success. */
static int load_helper(pTHX){
	eval_pv(helper_pm, FALSE) ;
	return (DIED ? -1 : 0) ;
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
	SV **gvp = hv_fetch(stash, name, key_len(name), 0) ;
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
	if (DIED){
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
	if (DIED){
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
		if ((SvTYPE(target) == SVt_PVAV)||is_iterator(aTHX_ sv)){
			t |= UGLY_TRAIT_ITERABLE ;
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
			SV **v = hv_fetch((HV *)target, name, member_len(aTHX_ (HV *)target, name), 0) ;
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
			(void)hv_store((HV *)SvRV(sv), name, member_len(aTHX_ (HV *)SvRV(sv), name), pv, 0) ;
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
		(void)hv_delete((HV *)SvRV(sv), name, member_len(aTHX_ (HV *)SvRV(sv), name), G_DISCARD) ;
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
			/*
			 * As UTF-8: a character key's own bytes; a byte key's too when
			 * they are UTF-8 already (undecoded text); otherwise its
			 * Latin-1 characters, encoded.
			 */
			STRLEN klen ;
			SV *ksv = hv_iterkeysv(he) ;
			const char *k = SvPV(ksv, klen) ;
			if ((! SvUTF8(ksv))&&(! valid_utf8((const unsigned char *)k, klen))){
				k = SvPVutf8(ksv, klen) ;
			}
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
		SV **v = hv_fetch((HV *)SvRV(sv), method, member_len(aTHX_ (HV *)SvRV(sv), method), 0) ;
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


static ugly_status pl_call_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_kind want, ugly_val *out){
	return call_entry(ctx, rt, ref, args, nargs, o, want, out) ;
}

static ugly_status pl_invoke_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *o, ugly_kind want, ugly_val *out){
	return invoke_entry(ctx, rt, ref, method, args, nargs, o, want, out) ;
}

static ugly_status pl_new_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *o, ugly_handle *out){
	return new_entry(ctx, rt, ref, args, nargs, o, out) ;
}


/*
 * Iteration (contract §2): an iterator is itself; an array gets an
 * Ugly::Iterator over it (helper.pm), an object whose next walks its
 * indices, so it sees the array as it is when each value is asked for.
 */
static ugly_status pl_iter(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st ;
	if (is_iterator(aTHX_ sv)){
		st = export_sv(aTHX_ ctx, r, sv, out) ;
	}
	else if (array_of(aTHX_ sv) != NULL){
		HV *hv = newHV() ;
		hv_stores(hv, "of", newSVsv(sv)) ;
		hv_stores(hv, "at", newSViv(0)) ;
		SV *it = sv_2mortal(newRV_noinc((SV *)hv)) ;
		sv_bless(it, gv_stashpv("Ugly::Iterator", GV_ADD)) ;
		st = export_sv(aTHX_ ctx, r, it, out) ;
	}
	else {
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not iterable (an array, or an object with a next method)") ;
	}
	LEAVE_PERL() ;
	return st ;
}


/* $it->next, in list context, up to max times: the empty list is the end; several values collapse into an array. */
static ugly_status pl_next(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_kind want, ugly_val *out, size_t max, size_t *got){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st = UGLY_OK ;
	size_t n = 0 ;
	if (! is_iterator(aTHX_ sv)){
		st = fail(ctx, r, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not an iterator (an object with a next method)") ;
	}
	while ((st == UGLY_OK)&&(n < max)){
		ENTER ;
		SAVETMPS ;
		dSP ;
		PUSHMARK(SP) ;
		XPUSHs(sv) ;
		PUTBACK ;
		int count = call_method("next", G_LIST | G_EVAL) ;
		SPAGAIN ;
		int end = 0 ;
		if (DIED){
			SP -= count ;
			PUTBACK ;
			st = perl_failure(aTHX_ ctx, r) ;
		}
		else if (count == 0){
			end = 1 ;
		}
		else {
			SV *result ;
			if (count == 1){
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
			st = to_val(aTHX_ ctx, r, result, want, &out[n]) ;
			if (st == UGLY_OK){
				n++ ;
			}
		}
		FREETMPS ;
		LEAVE ;
		if (end){
			break ;
		}
	}
	*got = n ;
	LEAVE_PERL() ;
	return st ;
}


/* "$x", overloading included; a package is its name. */
static ugly_status pl_to_string(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val *out){
	plrt *r = RT(rt) ;
	ENTER_PERL(r) ;
	SV *sv = ref ;
	ugly_status st ;
	ENTER ;
	SAVETMPS ;
	if (IS_PACKAGE(sv)){
		st = to_val(aTHX_ ctx, r, sv_2mortal(newSVpv(PACKAGE_NAME(sv), 0)), UGLY_ANY, out) ;
	}
	else {
		/* Through Perl, under eval: an overloaded "" may die. */
		dSP ;
		PUSHMARK(SP) ;
		XPUSHs(sv) ;
		PUTBACK ;
		int count = call_pv("Ugly::_str", G_SCALAR | G_EVAL) ;
		SPAGAIN ;
		if (DIED){
			SP -= count ;
			PUTBACK ;
			st = perl_failure(aTHX_ ctx, r) ;
		}
		else {
			SV *s = POPs ;
			PUTBACK ;
			st = to_val(aTHX_ ctx, r, s, UGLY_ANY, out) ;
		}
	}
	FREETMPS ;
	LEAVE ;
	LEAVE_PERL() ;
	return st ;
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


/* ---------------------------------------------------------------- */
/* Every entry runs under eval                                      */
/* ---------------------------------------------------------------- */

/*
 * Perl's API croaks: a store into a restricted hash or a read-only array, a
 * tied FETCH or STORE that dies, an overloaded bool or "". Outside any eval
 * that ends the process (an embedded perl) or jumps over the core's frames
 * to the host's eval (the binding). So each entry point below runs its
 * pl_* function through Ugly::_guard, an XS sub called with G_EVAL: a croak
 * comes back as $@, and becomes the entry's error like any die (rule 7).
 * What the entry had allocated in C at that point is lost.
 *
 * Calls are the exception, since they are the hot path and call_perl runs
 * the callee under eval already (the guard doubled a call's cost,
 * tests/bench.c): call, invoke and new go through the guard only when their
 * target can run code just by being used (runs_code: it's tied), and
 * call_perl converts a result under the guard when that can (tied, or an
 * overloaded object asked for as a primitive).
 */
typedef struct guard {
	ugly_status (*run)(struct guard *g) ;
	ugly_status st ;
	ugly_ctx *ctx ;
	ugly_rt *rt ;
	void *ref ;
	const char *name ;
	size_t len ;
	ugly_kind want ;
	ugly_val *out ;
	ugly_val v ;
	int64_t i, j ;
	int64_t *length ;
	const ugly_val *args ;
	size_t n ;
	const ugly_opts *opts ;
	uint32_t *traits ;
	ugly_handle *h ;
	size_t *got ;
} guard ;


static XS(xs_guard){
	dXSARGS ;
	PERL_UNUSED_VAR(items) ;
	plrt *r = R(aTHX) ;
	if ((r != NULL)&&(r->guard != NULL)){
		guard *g = r->guard ;
		g->st = g->run(g) ;
	}
	XSRETURN_EMPTY ;
}


static ugly_status protect(guard *g){
	plrt *r = RT(g->rt) ;
	PerlInterpreter *saved = PERL_GET_CONTEXT ;
	PerlInterpreter *my_perl = r->perl ;
	PERL_SET_CONTEXT(my_perl) ;
	if (r->guard_cv == NULL){
		r->guard_cv = get_cv("Ugly::_guard", 0) ;
	}
	guard *outer = r->guard ;
	r->guard = g ;
	g->st = UGLY_ERR_INTERNAL ;
	ENTER ;
	SAVETMPS ;
	dSP ;
	PUSHMARK(SP) ;
	PUTBACK ;
	call_sv((SV *)r->guard_cv, G_EVAL | G_DISCARD | G_NOARGS | G_VOID) ;
	r->guard = outer ;
	ugly_status st = g->st ;
	if (DIED){     /* a ref is an error whatever its bool overload says */
		st = perl_failure(aTHX_ g->ctx, r) ;
	}
	FREETMPS ;
	LEAVE ;
	PERL_SET_CONTEXT(saved) ;
	return st ;
}

#define GUARD(g_, run_, ctx_, rt_, ref_) guard g_ ; memset(&g_, 0, sizeof(g_)) ; g_.run = run_ ; g_.ctx = ctx_ ; g_.rt = rt_ ; g_.ref = ref_

static int target_runs_code(ugly_rt *rt, void *ref){
	(void)rt ;
	return runs_code((SV *)ref, 0) ;
}

static ugly_status run_to_val(guard *g){
	return to_val(aTHX_ g->ctx, RT(g->rt), (SV *)g->ref, g->want, g->out) ;     /* protect made the interpreter current */
}
static ugly_status to_val_guarded(pTHX_ ugly_ctx *ctx, plrt *r, SV *sv, ugly_kind want, ugly_val *out){
	GUARD(g, run_to_val, ctx, r->urt, sv) ; g.want = want ; g.out = out ;
	return protect(&g) ;
}

static ugly_status run_import(guard *g){ return pl_import(g->ctx, g->rt, g->name, g->out) ; }
static ugly_status g_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	GUARD(g, run_import, ctx, rt, NULL) ; g.name = name ; g.out = out ;
	return protect(&g) ;
}

static ugly_status run_globals(guard *g){ return pl_globals(g->ctx, g->rt, g->out) ; }
static ugly_status g_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	GUARD(g, run_globals, ctx, rt, NULL) ; g.out = out ;
	return protect(&g) ;
}

static ugly_status run_eval(guard *g){ return pl_eval(g->ctx, g->rt, g->name, g->len, g->want, g->out) ; }
static ugly_status g_eval(ugly_ctx *ctx, ugly_rt *rt, const char *code, size_t len, ugly_kind want, ugly_val *out){
	GUARD(g, run_eval, ctx, rt, NULL) ; g.name = code ; g.len = len ; g.want = want ; g.out = out ;
	return protect(&g) ;
}

static ugly_status run_describe(guard *g){ return pl_describe(g->ctx, g->rt, g->ref, g->traits, g->out) ; }
static ugly_status g_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	GUARD(g, run_describe, ctx, rt, ref) ; g.traits = traits ; g.out = type_name ;
	return protect(&g) ;
}

static ugly_status run_get_member(guard *g){ return pl_get_member(g->ctx, g->rt, g->ref, g->name, g->want, g->out) ; }
static ugly_status g_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out){
	GUARD(g, run_get_member, ctx, rt, ref) ; g.name = name ; g.want = want ; g.out = out ;
	return protect(&g) ;
}

static ugly_status run_set_member(guard *g){ return pl_set_member(g->ctx, g->rt, g->ref, g->name, g->v) ; }
static ugly_status g_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	GUARD(g, run_set_member, ctx, rt, ref) ; g.name = name ; g.v = v ;
	return protect(&g) ;
}

static ugly_status run_delete_member(guard *g){ return pl_delete_member(g->ctx, g->rt, g->ref, g->name) ; }
static ugly_status g_delete_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name){
	GUARD(g, run_delete_member, ctx, rt, ref) ; g.name = name ;
	return protect(&g) ;
}

static ugly_status run_member_names(guard *g){ return pl_member_names(g->ctx, g->rt, g->ref, g->h) ; }
static ugly_status g_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	GUARD(g, run_member_names, ctx, rt, ref) ; g.h = out ;
	return protect(&g) ;
}

static ugly_status run_length(guard *g){ return pl_length(g->ctx, g->rt, g->ref, g->length) ; }
static ugly_status g_length(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	GUARD(g, run_length, ctx, rt, ref) ; g.length = out ;
	return protect(&g) ;
}

static ugly_status run_get_index(guard *g){ return pl_get_index(g->ctx, g->rt, g->ref, g->i, g->want, g->out) ; }
static ugly_status g_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	GUARD(g, run_get_index, ctx, rt, ref) ; g.i = i ; g.want = want ; g.out = out ;
	return protect(&g) ;
}

static ugly_status run_set_index(guard *g){ return pl_set_index(g->ctx, g->rt, g->ref, g->i, g->v) ; }
static ugly_status g_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	GUARD(g, run_set_index, ctx, rt, ref) ; g.i = i ; g.v = v ;
	return protect(&g) ;
}

static ugly_status run_splice(guard *g){ return pl_splice(g->ctx, g->rt, g->ref, g->i, g->j, g->args, g->n) ; }
static ugly_status g_splice(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t offset, int64_t remove,
	const ugly_val *insert, size_t n){

	GUARD(g, run_splice, ctx, rt, ref) ; g.i = offset ; g.j = remove ; g.args = insert ; g.n = n ;
	return protect(&g) ;
}

static ugly_status run_call(guard *g){ return pl_call_opts(g->ctx, g->rt, g->ref, g->args, g->n, g->opts, g->want, g->out) ; }
static ugly_status g_call_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *opts, ugly_kind want, ugly_val *out){

	if (! target_runs_code(rt, ref)){
		return pl_call_opts(ctx, rt, ref, args, nargs, opts, want, out) ;
	}
	GUARD(g, run_call, ctx, rt, ref) ; g.args = args ; g.n = nargs ; g.opts = opts ; g.want = want ; g.out = out ;
	return protect(&g) ;
}
static ugly_status g_call(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){

	return g_call_opts(ctx, rt, ref, args, nargs, NULL, want, out) ;
}

static ugly_status run_invoke(guard *g){
	return pl_invoke_opts(g->ctx, g->rt, g->ref, g->name, g->args, g->n, g->opts, g->want, g->out) ;
}
static ugly_status g_invoke_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, const ugly_opts *opts, ugly_kind want, ugly_val *out){

	if (! target_runs_code(rt, ref)){
		return pl_invoke_opts(ctx, rt, ref, method, args, nargs, opts, want, out) ;
	}
	GUARD(g, run_invoke, ctx, rt, ref) ; g.name = method ; g.args = args ; g.n = nargs ; g.opts = opts ; g.want = want ; g.out = out ;
	return protect(&g) ;
}
static ugly_status g_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){

	return g_invoke_opts(ctx, rt, ref, method, args, nargs, NULL, want, out) ;
}

static ugly_status run_new(guard *g){ return pl_new_opts(g->ctx, g->rt, g->ref, g->args, g->n, g->opts, g->h) ; }
static ugly_status g_new_opts(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	const ugly_opts *opts, ugly_handle *out){

	if (! target_runs_code(rt, ref)){
		return pl_new_opts(ctx, rt, ref, args, nargs, opts, out) ;
	}
	GUARD(g, run_new, ctx, rt, ref) ; g.args = args ; g.n = nargs ; g.opts = opts ; g.h = out ;
	return protect(&g) ;
}
static ugly_status g_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){

	return g_new_opts(ctx, rt, ref, args, nargs, NULL, out) ;
}

static ugly_status run_iter(guard *g){ return pl_iter(g->ctx, g->rt, g->ref, g->h) ; }
static ugly_status g_iter(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	GUARD(g, run_iter, ctx, rt, ref) ; g.h = out ;
	return protect(&g) ;
}

static ugly_status run_next(guard *g){ return pl_next(g->ctx, g->rt, g->ref, g->want, g->out, g->n, g->got) ; }
static ugly_status g_next(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_kind want, ugly_val *out, size_t max, size_t *got){
	GUARD(g, run_next, ctx, rt, ref) ; g.want = want ; g.out = out ; g.n = max ; g.got = got ;
	return protect(&g) ;
}

static ugly_status run_to_string(guard *g){ return pl_to_string(g->ctx, g->rt, g->ref, g->out) ; }
static ugly_status g_to_string(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_val *out){
	GUARD(g, run_to_string, ctx, rt, ref) ; g.out = out ;
	return protect(&g) ;
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
	.import = g_import,
	.globals = g_globals,
	.eval = g_eval,
	.describe = g_describe,
	.get_member = g_get_member,
	.set_member = g_set_member,
	.member_names = g_member_names,
	.length = g_length,
	.get_index = g_get_index,
	.set_index = g_set_index,
	.call_opts = g_call_opts,
	.invoke_opts = g_invoke_opts,
	.new_opts = g_new_opts,
	.delete_member = g_delete_member,
	.splice = g_splice,
	.call = g_call,
	.invoke = g_invoke,
	.new_instance = g_new_instance,
	.release = pl_release,
	.iter = g_iter,
	.next = g_next,
	.to_string = g_to_string,
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
