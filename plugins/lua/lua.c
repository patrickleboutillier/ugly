/*
 * The Lua 5.4 plugin. Each runtime is a lua_State.
 *
 * - Native refs are registry references (luaL_ref), encoded as pointers.
 *   Identity is lua_topointer() for tables, functions, userdata and threads.
 * - Every operation runs its body under lua_pcall, so a Lua error never
 *   longjmps across the core: Lua code that fails becomes a ugly_error at
 *   the boundary.
 * - Foreign values appear in Lua as "ugly.proxy" userdata (one owned handle
 *   each; released by __gc), cached per handle in a weak table so the same
 *   foreign value is the same proxy (== holds). Proxies unwrap to their
 *   handle on the way out.
 * - A ugly_error raised into Lua travels as an "ugly.error" userdata. If it
 *   escapes Lua unhandled, the original error is restored unchanged; if Lua
 *   code reads e.payload, it gets the original exception object.
 * - Elements are 0-based in the protocol and 1-based in Lua: ugly index i is
 *   Lua index i + 1, in both directions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "ugly_plugin.h"


#define PROXY_MT "ugly.proxy"
#define ERROR_MT "ugly.error"
#define PROXY_CACHE "ugly.proxies"

#define REF(p) ((int)(intptr_t)(p))
#define PTR(r) ((void *)(intptr_t)(r))

typedef struct lrt {
	lua_State *L ;
	ugly_rt *urt ;
} lrt ;

typedef struct { ugly_handle h ; } proxy ;
typedef struct { ugly_error *err ; } errbox ;


static lrt *R(lua_State *L){
	lrt *r ;
	memcpy(&r, lua_getextraspace(L), sizeof(r)) ;
	return r ;
}


/* ---------------------------------------------------------------- */
/* Strings                                                          */
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
			return 0 ;     /* truncated sequence */
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
			return 0 ;     /* overlong, out of range, or a surrogate */
		}
		i += n + 1 ;
	}
	return 1 ;
}


/* ---------------------------------------------------------------- */
/* Lua -> ugly_val                                                  */
/* ---------------------------------------------------------------- */

static ugly_status type_error(ugly_ctx *ctx, lrt *r, const char *what, int idx){
	char msg[160] ;
	snprintf(msg, sizeof(msg), "can't convert a Lua %s to %s", luaL_typename(r->L, idx), what) ;
	return ugly_fail(ctx, r->urt, UGLY_ERR_TYPE, "TypeError", msg) ;
}


/* Exports the Lua value at idx as an owned handle (unwrapping our proxies). */
static ugly_status export(ugly_ctx *ctx, lrt *r, int idx, ugly_handle *out){
	lua_State *L = r->L ;
	proxy *p = luaL_testudata(L, idx, PROXY_MT) ;
	if (p != NULL){
		return ugly_dup(ctx, p->h, out) ;
	}
	uintptr_t identity = (uintptr_t)lua_topointer(L, idx) ;
	lua_pushvalue(L, idx) ;
	int ref = luaL_ref(L, LUA_REGISTRYINDEX) ;
	return ugly_export(ctx, r->urt, PTR(ref), identity, out) ;
}


/* The Lua value at idx as a result of kind 'want', by Lua's own conversion rules. */
static ugly_status to_val(ugly_ctx *ctx, lrt *r, int idx, ugly_kind want, ugly_val *out){
	lua_State *L = r->L ;
	idx = lua_absindex(L, idx) ;
	int t = lua_type(L, idx) ;
	*out = ugly_null() ;

	if (want == UGLY_ANY){
		switch (t){
			case LUA_TNIL: case LUA_TNONE: return UGLY_OK ;
			case LUA_TBOOLEAN: want = UGLY_BOOL ; break ;
			case LUA_TNUMBER: want = (lua_isinteger(L, idx) ? UGLY_INT : UGLY_DOUBLE) ; break ;
			case LUA_TSTRING: {
				size_t len ;
				const char *s = lua_tolstring(L, idx, &len) ;
				want = (valid_utf8((const unsigned char *)s, len) ? UGLY_STRING : UGLY_BYTES) ;
				break ;
			}
			default: want = UGLY_HANDLE ; break ;
		}
	}

	switch (want){
		case UGLY_NULL:
			return (t == LUA_TNIL ? UGLY_OK : type_error(ctx, r, "null", idx)) ;
		case UGLY_BOOL:
			/* Lua's truthiness: only nil and false are false. */
			*out = ugly_bool(lua_toboolean(L, idx)) ;
			return UGLY_OK ;
		case UGLY_INT: {
			int isnum = 0 ;
			lua_Integer i = lua_tointegerx(L, idx, &isnum) ;    /* exact floats and numeric strings convert */
			if (! isnum){
				return type_error(ctx, r, "an integer", idx) ;
			}
			*out = ugly_int((int64_t)i) ;
			return UGLY_OK ;
		}
		case UGLY_DOUBLE: {
			int isnum = 0 ;
			lua_Number d = lua_tonumberx(L, idx, &isnum) ;
			if (! isnum){
				return type_error(ctx, r, "a double", idx) ;
			}
			*out = ugly_double((double)d) ;
			return UGLY_OK ;
		}
		case UGLY_STRING:
		case UGLY_BYTES: {
			if ((t != LUA_TSTRING)&&(t != LUA_TNUMBER)){
				return type_error(ctx, r, (want == UGLY_STRING ? "a string" : "bytes"), idx) ;
			}
			lua_pushvalue(L, idx) ;                /* lua_tolstring converts numbers in place: use a copy */
			size_t len ;
			const char *s = lua_tolstring(L, -1, &len) ;
			ugly_status st ;
			if (want == UGLY_STRING){
				st = (valid_utf8((const unsigned char *)s, len)
					? ugly_val_set_string(ctx, out, s, len)
					: ugly_fail(ctx, r->urt, UGLY_ERR_TYPE, "TypeError", "string is not valid UTF-8")) ;
			}
			else {
				st = ugly_val_set_bytes(ctx, out, (const uint8_t *)s, len) ;
			}
			lua_pop(L, 1) ;
			return st ;
		}
		case UGLY_HANDLE: {
			if ((t != LUA_TTABLE)&&(t != LUA_TFUNCTION)&&(t != LUA_TUSERDATA)&&(t != LUA_TTHREAD)){
				return type_error(ctx, r, "a handle", idx) ;
			}
			ugly_handle h ;
			ugly_status st = export(ctx, r, idx, &h) ;
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
/* ugly_val -> Lua                                                  */
/* ---------------------------------------------------------------- */

/* Pushes a proxy for a foreign handle: one per value (cached), owning one reference. */
static ugly_status push_proxy(ugly_ctx *ctx, lrt *r, ugly_handle borrowed){
	lua_State *L = r->L ;
	ugly_handle h ;
	ugly_status st = ugly_dup(ctx, borrowed, &h) ;     /* the owned handle: the same id for the same value */
	if (st != UGLY_OK){
		return st ;
	}
	lua_getfield(L, LUA_REGISTRYINDEX, PROXY_CACHE) ;
	lua_rawgetp(L, -1, (void *)(uintptr_t)h) ;
	if (! lua_isnil(L, -1)){
		lua_remove(L, -2) ;
		ugly_release(ctx, h) ;    /* the cached proxy already holds a reference */
		return UGLY_OK ;
	}
	lua_pop(L, 1) ;
	proxy *p = lua_newuserdatauv(L, sizeof(proxy), 0) ;
	p->h = h ;
	luaL_setmetatable(L, PROXY_MT) ;
	lua_pushvalue(L, -1) ;
	lua_rawsetp(L, -3, (void *)(uintptr_t)h) ;
	lua_remove(L, -2) ;
	return UGLY_OK ;
}


/* Pushes an argument (borrowed): our own values resolve to the original (rule 5), foreign ones get a proxy. */
static ugly_status push_val(ugly_ctx *ctx, lrt *r, ugly_val v){
	lua_State *L = r->L ;
	switch (v.kind){
		case UGLY_NULL: lua_pushnil(L) ; return UGLY_OK ;
		case UGLY_BOOL: lua_pushboolean(L, v.as.b) ; return UGLY_OK ;
		case UGLY_INT: lua_pushinteger(L, (lua_Integer)v.as.i) ; return UGLY_OK ;
		case UGLY_DOUBLE: lua_pushnumber(L, (lua_Number)v.as.d) ; return UGLY_OK ;
		case UGLY_STRING: lua_pushlstring(L, v.as.str.ptr, v.as.str.len) ; return UGLY_OK ;
		case UGLY_BYTES: lua_pushlstring(L, (const char *)v.as.bytes.ptr, v.as.bytes.len) ; return UGLY_OK ;
		case UGLY_HANDLE: {
			void *ref = NULL ;
			ugly_status st = ugly_resolve(ctx, r->urt, v.as.h, &ref) ;
			if (st != UGLY_OK){
				return st ;
			}
			if (ref != NULL){
				lua_rawgeti(L, LUA_REGISTRYINDEX, REF(ref)) ;
				return UGLY_OK ;
			}
			return push_proxy(ctx, r, v.as.h) ;
		}
		default:
			return UGLY_ERR_USAGE ;
	}
}


/* ---------------------------------------------------------------- */
/* Errors at the boundary                                           */
/* ---------------------------------------------------------------- */

/* Raises the ctx's current error in Lua, as an ugly.error. Never returns. */
static int throw_ugly_error(lua_State *L, ugly_ctx *ctx, ugly_status st){
	if (ugly_last_error(ctx) == NULL){
		/* A failure that recorded no error (e.g. a stale handle met while converting). */
		ugly_fail(ctx, R(L)->urt, st, "Error", "operation failed") ;
	}
	errbox *b = lua_newuserdatauv(L, sizeof(errbox), 0) ;
	b->err = ugly_take_error(ctx) ;
	luaL_setmetatable(L, ERROR_MT) ;
	return lua_error(L) ;
}


/* The Lua error value at the top of the stack, escaping to the core (popped). */
static ugly_status lua_failure(ugly_ctx *ctx, lrt *r){
	lua_State *L = r->L ;

	/* Our own wrapper: the original error goes back out unchanged (rule 7). */
	errbox *b = luaL_testudata(L, -1, ERROR_MT) ;
	if ((b != NULL)&&(b->err != NULL)){
		ugly_error *e = b->err ;
		b->err = NULL ;
		lua_pop(L, 1) ;
		return ugly_restore_error(ctx, e) ;
	}

	/* type: the metatable's __name, or "error" for a string, or the Lua type. */
	char type[64], message[512] ;
	if (luaL_getmetafield(L, -1, "__name") == LUA_TSTRING){
		snprintf(type, sizeof(type), "%s", lua_tostring(L, -1)) ;
		lua_pop(L, 1) ;
	}
	else {
		snprintf(type, sizeof(type), "%s", (lua_type(L, -1) == LUA_TSTRING ? "error" : luaL_typename(L, -1))) ;
	}

	/* message: the string itself, or a 'message' field (raw: no metamethods outside a pcall), or the type. */
	snprintf(message, sizeof(message), "%s", type) ;
	if (lua_type(L, -1) == LUA_TSTRING){
		snprintf(message, sizeof(message), "%s", lua_tostring(L, -1)) ;
	}
	else if (lua_type(L, -1) == LUA_TTABLE){
		lua_pushliteral(L, "message") ;
		if (lua_rawget(L, -2) == LUA_TSTRING){
			snprintf(message, sizeof(message), "%s", lua_tostring(L, -1)) ;
		}
		lua_pop(L, 1) ;
	}

	/* A rethrown proxy: the payload is the foreign value itself. */
	proxy *p = luaL_testudata(L, -1, PROXY_MT) ;
	if (p != NULL){
		ugly_handle h ;
		ugly_status st = ugly_dup(ctx, p->h, &h) ;
		lua_pop(L, 1) ;
		if (st != UGLY_OK){
			h = UGLY_NO_HANDLE ;
		}
		return ugly_raise_handle(ctx, r->urt, type, message, h) ;
	}

	/* Anything else: the error value is the payload. */
	if (lua_isnil(L, -1)){
		lua_pop(L, 1) ;
		return ugly_raise_native(ctx, r->urt, type, message, NULL, 0) ;
	}
	int t = lua_type(L, -1) ;
	uintptr_t identity = (((t == LUA_TTABLE)||(t == LUA_TUSERDATA)||(t == LUA_TFUNCTION)||(t == LUA_TTHREAD))
		? (uintptr_t)lua_topointer(L, -1) : 0) ;
	int ref = luaL_ref(L, LUA_REGISTRYINDEX) ;
	return ugly_raise_native(ctx, r->urt, type, message, PTR(ref), identity) ;
}


/* ---------------------------------------------------------------- */
/* Operations, each run under lua_pcall                             */
/* ---------------------------------------------------------------- */

enum { OP_IMPORT, OP_GLOBALS, OP_EVAL, OP_DESCRIBE, OP_GET_MEMBER, OP_SET_MEMBER, OP_MEMBER_NAMES,
	OP_LENGTH, OP_GET_INDEX, OP_SET_INDEX, OP_INVOKE, OP_NEW } ;

typedef struct op {
	int code ;
	ugly_ctx *ctx ;
	lrt *r ;
	void *ref ;
	const char *name ;
	size_t len ;
	int64_t index ;
	const ugly_val *args ;
	size_t nargs ;
	ugly_kind want ;
	ugly_val *out ;
	ugly_val v ;
	uint32_t *traits ;
	int64_t *length ;
	ugly_handle *hout ;
	ugly_status st ;      /* set by the body for failures that aren't Lua errors */
} op ;


#define FAIL(o, status, type, msg) do { (o)->st = ugly_fail((o)->ctx, (o)->r->urt, (status), (type), (msg)) ; return 0 ; } while (0)
#define CHECK(o, expr) do { if (((o)->st = (expr)) != UGLY_OK){ return 0 ; } } while (0)

static int indexable(lua_State *L, int idx){
	if (lua_type(L, idx) == LUA_TTABLE){
		return 1 ;
	}
	if (lua_type(L, idx) == LUA_TUSERDATA){
		int has = (luaL_getmetafield(L, idx, "__index") != LUA_TNIL) ;
		if (has){
			lua_pop(L, 1) ;
		}
		return has ;
	}
	return 0 ;
}


static int callable(lua_State *L, int idx){
	if (lua_type(L, idx) == LUA_TFUNCTION){
		return 1 ;
	}
	int has = (luaL_getmetafield(L, idx, "__call") != LUA_TNIL) ;
	if (has){
		lua_pop(L, 1) ;
	}
	return has ;
}


static int push_args(op *o){
	size_t i ;
	for (i = 0 ; i < o->nargs ; i++){
		if ((o->st = push_val(o->ctx, o->r, o->args[i])) != UGLY_OK){
			return 0 ;
		}
	}
	return 1 ;
}


static int op_body(lua_State *L){
	op *o = lua_touserdata(L, 1) ;
	lrt *r = o->r ;
	ugly_ctx *ctx = o->ctx ;
	o->st = UGLY_OK ;
	if (o->ref != NULL){
		lua_rawgeti(L, LUA_REGISTRYINDEX, REF(o->ref)) ;     /* the target, at index 2 */
	}

	switch (o->code){
		case OP_IMPORT:
			lua_getglobal(L, "require") ;
			lua_pushstring(L, o->name) ;
			lua_call(L, 1, 1) ;
			if (! indexable(L, -1) && ! callable(L, -1)){
				FAIL(o, UGLY_ERR_TYPE, "ImportError", "the module is not a table") ;
			}
			o->st = to_val(ctx, r, -1, UGLY_HANDLE, o->out) ;
			return 0 ;

		case OP_GLOBALS:
			lua_pushglobaltable(L) ;
			o->st = to_val(ctx, r, -1, UGLY_HANDLE, o->out) ;
			return 0 ;

		case OP_EVAL:
			if (luaL_loadbuffer(L, o->name, o->len, "=eval") != LUA_OK){
				return lua_error(L) ;
			}
			lua_call(L, 0, 1) ;
			o->st = to_val(ctx, r, -1, o->want, o->out) ;
			return 0 ;

		case OP_DESCRIBE: {
			uint32_t t = 0 ;
			int lt = lua_type(L, 2) ;
			if (lt == LUA_TTABLE){
				t |= UGLY_TRAIT_MEMBERS | UGLY_TRAIT_ELEMENTS ;
				if (lua_getfield(L, 2, "new") == LUA_TFUNCTION){
					t |= UGLY_TRAIT_INSTANTIABLE ;
				}
				lua_pop(L, 1) ;
			}
			else if ((lt == LUA_TUSERDATA)&&(indexable(L, 2))){
				t |= UGLY_TRAIT_MEMBERS ;
			}
			if (callable(L, 2)){
				t |= UGLY_TRAIT_CALLABLE ;
			}
			*o->traits = t ;
			const char *name = luaL_typename(L, 2) ;
			if (luaL_getmetafield(L, 2, "__name") == LUA_TSTRING){
				name = lua_tostring(L, -1) ;
			}
			o->st = ugly_val_set_string(ctx, o->out, name, strlen(name)) ;
			return 0 ;
		}

		case OP_GET_MEMBER:
			if (! indexable(L, 2)){
				FAIL(o, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no members") ;
			}
			if (lua_getfield(L, 2, o->name) == LUA_TNIL){
				FAIL(o, UGLY_ERR_NOT_FOUND, "KeyError", o->name) ;
			}
			o->st = to_val(ctx, r, -1, o->want, o->out) ;
			return 0 ;

		case OP_SET_MEMBER:
			if (! indexable(L, 2)){
				FAIL(o, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no members") ;
			}
			CHECK(o, push_val(ctx, r, o->v)) ;
			lua_setfield(L, 2, o->name) ;
			return 0 ;

		case OP_MEMBER_NAMES: {
			if (lua_type(L, 2) != LUA_TTABLE){
				FAIL(o, UGLY_ERR_NOT_CAPABLE, "TypeError", "only tables list their members") ;
			}
			size_t n = 0, cap = 16 ;
			ugly_val *names = malloc(cap * sizeof(ugly_val)) ;
			lua_pushnil(L) ;
			while (lua_next(L, 2) != 0){
				if (lua_type(L, -2) == LUA_TSTRING){
					if (n == cap){
						cap *= 2 ;
						names = realloc(names, cap * sizeof(ugly_val)) ;
					}
					/* The key string is anchored by the table itself, so the pointer stays valid. */
					size_t len ;
					const char *k = lua_tolstring(L, -2, &len) ;
					names[n++] = ugly_str(k, len) ;
				}
				lua_pop(L, 1) ;
			}
			o->st = ugly_list_new(ctx, names, n, o->hout) ;
			free(names) ;
			return 0 ;
		}

		case OP_LENGTH:
			if ((lua_type(L, 2) != LUA_TTABLE)&&(luaL_getmetafield(L, 2, "__len") == LUA_TNIL)){
				FAIL(o, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no length") ;
			}
			*o->length = (int64_t)luaL_len(L, 2) ;
			return 0 ;

		case OP_GET_INDEX:
		case OP_SET_INDEX: {
			if (lua_type(L, 2) != LUA_TTABLE){
				FAIL(o, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no elements") ;
			}
			lua_Integer len = luaL_len(L, 2) ;
			lua_Integer last = (o->code == OP_GET_INDEX ? len - 1 : len) ;     /* set may append */
			if ((o->index < 0)||(o->index > last)){
				FAIL(o, UGLY_ERR_NOT_FOUND, "IndexError", "index out of range") ;
			}
			if (o->code == OP_GET_INDEX){
				lua_geti(L, 2, (lua_Integer)o->index + 1) ;
				o->st = to_val(ctx, r, -1, o->want, o->out) ;
			}
			else {
				CHECK(o, push_val(ctx, r, o->v)) ;
				lua_seti(L, 2, (lua_Integer)o->index + 1) ;
			}
			return 0 ;
		}

		case OP_INVOKE: {
			/*
			 * Lua's convention: a method found through the metatable is
			 * obj:method(args), with the receiver first; a plain field
			 * (a module's function) is obj.f(args), without it.
			 */
			if (! indexable(L, 2)){
				FAIL(o, UGLY_ERR_NOT_CAPABLE, "TypeError", "value has no methods") ;
			}
			int plain = 0 ;
			if (lua_type(L, 2) == LUA_TTABLE){
				lua_pushstring(L, o->name) ;
				plain = (lua_rawget(L, 2) != LUA_TNIL) ;
				lua_pop(L, 1) ;
			}
			if (lua_getfield(L, 2, o->name) == LUA_TNIL){
				FAIL(o, UGLY_ERR_NOT_FOUND, "KeyError", o->name) ;
			}
			if (! plain){
				lua_pushvalue(L, 2) ;
			}
			if (! push_args(o)){
				return 0 ;
			}
			lua_call(L, (int)o->nargs + (plain ? 0 : 1), 1) ;
			o->st = to_val(ctx, r, -1, o->want, o->out) ;
			return 0 ;
		}

		case OP_NEW:
			/* Class.new(args), or Class(args) for a callable class. */
			if (indexable(L, 2)&&(lua_getfield(L, 2, "new") == LUA_TFUNCTION)){
				/* the constructor is on the stack */
			}
			else if (callable(L, 2)){
				lua_settop(L, 2) ;
			}
			else {
				FAIL(o, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not instantiable") ;
			}
			if (! push_args(o)){
				return 0 ;
			}
			lua_call(L, (int)o->nargs, 1) ;
			{
				ugly_val v ;
				o->st = to_val(ctx, r, -1, UGLY_HANDLE, &v) ;
				if (o->st == UGLY_OK){
					*o->hout = v.as.h ;
				}
			}
			return 0 ;
	}
	return 0 ;
}


static ugly_status run(op *o){
	lua_State *L = o->r->L ;
	int top = lua_gettop(L) ;
	lua_pushcfunction(L, op_body) ;
	lua_pushlightuserdata(L, o) ;
	ugly_status st ;
	if (lua_pcall(L, 1, 0, 0) != LUA_OK){
		st = lua_failure(o->ctx, o->r) ;
	}
	else {
		st = o->st ;
	}
	lua_settop(L, top) ;
	return st ;
}


static op mkop(int code, ugly_ctx *ctx, ugly_rt *rt, void *ref){
	op o ;
	memset(&o, 0, sizeof(o)) ;
	o.code = code ;
	o.ctx = ctx ;
	o.r = ugly_rt_native(rt) ;
	o.ref = ref ;
	return o ;
}


/* ---------------------------------------------------------------- */
/* Proxies: foreign values inside Lua                               */
/* ---------------------------------------------------------------- */

static ugly_ctx *need_ctx(lua_State *L){
	ugly_ctx *ctx = ugly_current_ctx() ;
	if (ctx == NULL){
		luaL_error(L, "ugly: no ctx on this thread") ;
	}
	return ctx ;
}


/* Converts Lua values [from, top] to owned ugly_vals. Returns how many were made (all, on success). */
static int lua_args(lua_State *L, ugly_ctx *ctx, int from, ugly_val *args, int n, ugly_status *st){
	int i ;
	for (i = 0 ; i < n ; i++){
		*st = to_val(ctx, R(L), from + i, UGLY_ANY, &args[i]) ;
		if (*st != UGLY_OK){
			return i ;
		}
	}
	*st = UGLY_OK ;
	return n ;
}


static void free_args(ugly_ctx *ctx, ugly_val *args, int n){
	int i ;
	for (i = 0 ; i < n ; i++){
		ugly_val_release(ctx, &args[i]) ;
	}
	free(args) ;
}


/* Pushes a result and releases it; raises the ctx's error in Lua on failure. */
static int finish(lua_State *L, ugly_ctx *ctx, ugly_status st, ugly_val *out){
	if (st != UGLY_OK){
		return throw_ugly_error(L, ctx, st) ;
	}
	st = push_val(ctx, R(L), *out) ;
	ugly_val_release(ctx, out) ;
	if (st != UGLY_OK){
		return throw_ugly_error(L, ctx, st) ;
	}
	return 1 ;
}


static int proxy_call(lua_State *L){
	proxy *p = luaL_checkudata(L, 1, PROXY_MT) ;
	ugly_ctx *ctx = need_ctx(L) ;
	int n = lua_gettop(L) - 1 ;
	ugly_val *args = calloc((size_t)n + 1, sizeof(ugly_val)) ;
	ugly_status st ;
	int made = lua_args(L, ctx, 2, args, n, &st) ;
	ugly_val out = ugly_null() ;
	if (st == UGLY_OK){
		st = ugly_call(ctx, p->h, args, (size_t)n, UGLY_ANY, &out) ;
	}
	free_args(ctx, args, made) ;
	return finish(L, ctx, st, &out) ;
}


/* obj:method(...) on a proxy: invoke with the foreign language's receiver semantics. */
static int proxy_method(lua_State *L){
	proxy *p = lua_touserdata(L, lua_upvalueindex(1)) ;
	const char *method = lua_tostring(L, lua_upvalueindex(2)) ;
	ugly_ctx *ctx = need_ctx(L) ;
	int first = 1 ;
	if ((lua_gettop(L) >= 1)&&(lua_rawequal(L, 1, lua_upvalueindex(1)))){
		first = 2 ;     /* called with ':': the receiver isn't an argument */
	}
	int n = lua_gettop(L) - first + 1 ;
	ugly_val *args = calloc((size_t)n + 1, sizeof(ugly_val)) ;
	ugly_status st ;
	int made = lua_args(L, ctx, first, args, n, &st) ;
	ugly_val out = ugly_null() ;
	if (st == UGLY_OK){
		st = ugly_invoke(ctx, p->h, method, args, (size_t)n, UGLY_ANY, &out) ;
	}
	free_args(ctx, args, made) ;
	return finish(L, ctx, st, &out) ;
}


static int proxy_index(lua_State *L){
	proxy *p = luaL_checkudata(L, 1, PROXY_MT) ;
	ugly_ctx *ctx = need_ctx(L) ;
	ugly_val out = ugly_null() ;
	ugly_status st ;
	if (lua_isinteger(L, 2)){
		st = ugly_get_index(ctx, p->h, (int64_t)lua_tointeger(L, 2) - 1, UGLY_ANY, &out) ;
		if (st == UGLY_ERR_NOT_FOUND){
			lua_pushnil(L) ;
			return 1 ;
		}
		return finish(L, ctx, st, &out) ;
	}
	const char *name = luaL_checkstring(L, 2) ;
	st = ugly_get_member(ctx, p->h, name, UGLY_ANY, &out) ;
	int method = ((st == UGLY_ERR_NOT_FOUND)||(st == UGLY_ERR_NOT_CAPABLE)) ;
	if ((st == UGLY_OK)&&(out.kind == UGLY_HANDLE)){
		/* A callable member is a method: invoke it, with the foreign language's receiver rules. */
		uint32_t traits = 0 ;
		ugly_val tn ;
		if ((ugly_describe(ctx, out.as.h, &traits, &tn) == UGLY_OK)&&(traits & UGLY_TRAIT_CALLABLE)){
			method = 1 ;
			ugly_val_release(ctx, &out) ;
		}
		ugly_val_release(ctx, &tn) ;
	}
	if (method){
		/* Called as obj:name(...) or obj.name(...): either way, invoke name on obj. */
		lua_pushvalue(L, 1) ;
		lua_pushstring(L, name) ;
		lua_pushcclosure(L, proxy_method, 2) ;
		return 1 ;
	}
	return finish(L, ctx, st, &out) ;
}


static int proxy_newindex(lua_State *L){
	proxy *p = luaL_checkudata(L, 1, PROXY_MT) ;
	ugly_ctx *ctx = need_ctx(L) ;
	ugly_val v ;
	ugly_status st = to_val(ctx, R(L), 3, UGLY_ANY, &v) ;
	if (st == UGLY_OK){
		if (lua_isinteger(L, 2)){
			st = ugly_set_index(ctx, p->h, (int64_t)lua_tointeger(L, 2) - 1, v) ;
		}
		else {
			st = ugly_set_member(ctx, p->h, luaL_checkstring(L, 2), v) ;
		}
		ugly_val_release(ctx, &v) ;
	}
	if (st != UGLY_OK){
		return throw_ugly_error(L, ctx, st) ;
	}
	return 0 ;
}


static int proxy_len(lua_State *L){
	proxy *p = luaL_checkudata(L, 1, PROXY_MT) ;
	ugly_ctx *ctx = need_ctx(L) ;
	int64_t n = 0 ;
	ugly_status st = ugly_length(ctx, p->h, &n) ;
	if (st != UGLY_OK){
		return throw_ugly_error(L, ctx, st) ;
	}
	lua_pushinteger(L, (lua_Integer)n) ;
	return 1 ;
}


static int proxy_eq(lua_State *L){
	proxy *a = luaL_testudata(L, 1, PROXY_MT) ;
	proxy *b = luaL_testudata(L, 2, PROXY_MT) ;
	bool same = false ;
	if ((a != NULL)&&(b != NULL)){
		ugly_same(need_ctx(L), a->h, b->h, &same) ;
	}
	lua_pushboolean(L, same) ;
	return 1 ;
}


static int proxy_tostring(lua_State *L){
	proxy *p = luaL_checkudata(L, 1, PROXY_MT) ;
	lua_pushfstring(L, "ugly.proxy(%p)", (void *)(uintptr_t)p->h) ;
	return 1 ;
}


static int proxy_gc(lua_State *L){
	proxy *p = luaL_checkudata(L, 1, PROXY_MT) ;
	if (p->h != UGLY_NO_HANDLE){
		ugly_release(ugly_current_ctx(), p->h) ;
		p->h = UGLY_NO_HANDLE ;
	}
	return 0 ;
}


/* ---------------------------------------------------------------- */
/* Error wrappers: e.type, e.message, e.language, e.payload         */
/* ---------------------------------------------------------------- */

static int error_index(lua_State *L){
	errbox *b = luaL_checkudata(L, 1, ERROR_MT) ;
	const char *k = luaL_checkstring(L, 2) ;
	if (b->err == NULL){
		lua_pushnil(L) ;
		return 1 ;
	}
	if (strcmp(k, "type") == 0){ lua_pushstring(L, ugly_error_type(b->err)) ; return 1 ; }
	if (strcmp(k, "message") == 0){ lua_pushstring(L, ugly_error_message(b->err)) ; return 1 ; }
	if (strcmp(k, "language") == 0){ lua_pushstring(L, ugly_error_language(b->err)) ; return 1 ; }
	if (strcmp(k, "payload") == 0){
		ugly_ctx *ctx = need_ctx(L) ;
		ugly_handle h = UGLY_NO_HANDLE ;
		if ((ugly_error_payload(ctx, b->err, &h) != UGLY_OK)||(h == UGLY_NO_HANDLE)){
			lua_pushnil(L) ;
			return 1 ;
		}
		ugly_status st = push_val(ctx, R(L), ugly_hval(h)) ;
		ugly_release(ctx, h) ;
		if (st != UGLY_OK){
			lua_pushnil(L) ;
		}
		return 1 ;
	}
	lua_pushnil(L) ;
	return 1 ;
}


static int error_tostring(lua_State *L){
	errbox *b = luaL_checkudata(L, 1, ERROR_MT) ;
	if (b->err == NULL){
		lua_pushliteral(L, "ugly.error") ;
	}
	else {
		lua_pushfstring(L, "[%s] %s: %s", ugly_error_language(b->err), ugly_error_type(b->err),
			ugly_error_message(b->err)) ;
	}
	return 1 ;
}


static int error_gc(lua_State *L){
	errbox *b = luaL_checkudata(L, 1, ERROR_MT) ;
	ugly_error_free(b->err) ;
	b->err = NULL ;
	return 0 ;
}


/* ---------------------------------------------------------------- */
/* The plugin                                                       */
/* ---------------------------------------------------------------- */

static ugly_status lua_rt_new(ugly_ctx *ctx, ugly_rt *rt, void **native){
	lrt *r = calloc(1, sizeof(lrt)) ;
	if (r == NULL){
		return UGLY_ERR_NOMEM ;
	}
	lua_State *L = luaL_newstate() ;
	if (L == NULL){
		free(r) ;
		return ugly_fail(ctx, rt, UGLY_ERR_NOMEM, "MemoryError", "can't create a Lua state") ;
	}
	r->L = L ;
	r->urt = rt ;
	memcpy(lua_getextraspace(L), &r, sizeof(r)) ;
	luaL_openlibs(L) ;

	static const luaL_Reg proxy_mt[] = {
		{ "__index", proxy_index }, { "__newindex", proxy_newindex }, { "__call", proxy_call },
		{ "__len", proxy_len }, { "__eq", proxy_eq }, { "__tostring", proxy_tostring }, { "__gc", proxy_gc },
		{ NULL, NULL }
	} ;
	luaL_newmetatable(L, PROXY_MT) ;
	luaL_setfuncs(L, proxy_mt, 0) ;
	lua_pop(L, 1) ;

	static const luaL_Reg error_mt[] = {
		{ "__index", error_index }, { "__tostring", error_tostring }, { "__gc", error_gc }, { NULL, NULL }
	} ;
	luaL_newmetatable(L, ERROR_MT) ;
	luaL_setfuncs(L, error_mt, 0) ;
	lua_pop(L, 1) ;

	/* Proxy cache: handle -> proxy, with weak values. */
	lua_newtable(L) ;
	lua_newtable(L) ;
	lua_pushliteral(L, "v") ;
	lua_setfield(L, -2, "__mode") ;
	lua_setmetatable(L, -2) ;
	lua_setfield(L, LUA_REGISTRYINDEX, PROXY_CACHE) ;

	*native = r ;
	return UGLY_OK ;
}


static ugly_status lua_rt_delete(ugly_ctx *ctx, ugly_rt *rt, void *native){
	(void)ctx ; (void)rt ;
	lrt *r = native ;
	lua_close(r->L) ;     /* runs __gc: proxies release their handles */
	free(r) ;
	return UGLY_OK ;
}


static ugly_status lua_import(ugly_ctx *ctx, ugly_rt *rt, const char *name, ugly_val *out){
	op o = mkop(OP_IMPORT, ctx, rt, NULL) ;
	o.name = name ;
	o.out = out ;
	return run(&o) ;
}


static ugly_status lua_globals(ugly_ctx *ctx, ugly_rt *rt, ugly_val *out){
	op o = mkop(OP_GLOBALS, ctx, rt, NULL) ;
	o.out = out ;
	return run(&o) ;
}


static ugly_status lua_eval(ugly_ctx *ctx, ugly_rt *rt, const char *code, size_t len, ugly_kind want, ugly_val *out){
	op o = mkop(OP_EVAL, ctx, rt, NULL) ;
	o.name = code ;
	o.len = len ;
	o.want = want ;
	o.out = out ;
	return run(&o) ;
}


static ugly_status lua_describe(ugly_ctx *ctx, ugly_rt *rt, void *ref, uint32_t *traits, ugly_val *type_name){
	op o = mkop(OP_DESCRIBE, ctx, rt, ref) ;
	o.traits = traits ;
	o.out = type_name ;
	return run(&o) ;
}


static ugly_status lua_get_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_kind want, ugly_val *out){
	op o = mkop(OP_GET_MEMBER, ctx, rt, ref) ;
	o.name = name ;
	o.want = want ;
	o.out = out ;
	return run(&o) ;
}


static ugly_status lua_set_member(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *name, ugly_val v){
	op o = mkop(OP_SET_MEMBER, ctx, rt, ref) ;
	o.name = name ;
	o.v = v ;
	return run(&o) ;
}


static ugly_status lua_member_names(ugly_ctx *ctx, ugly_rt *rt, void *ref, ugly_handle *out){
	op o = mkop(OP_MEMBER_NAMES, ctx, rt, ref) ;
	o.hout = out ;
	return run(&o) ;
}


static ugly_status lua_length_op(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t *out){
	op o = mkop(OP_LENGTH, ctx, rt, ref) ;
	o.length = out ;
	return run(&o) ;
}


static ugly_status lua_get_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_kind want, ugly_val *out){
	op o = mkop(OP_GET_INDEX, ctx, rt, ref) ;
	o.index = i ;
	o.want = want ;
	o.out = out ;
	return run(&o) ;
}


static ugly_status lua_set_index(ugly_ctx *ctx, ugly_rt *rt, void *ref, int64_t i, ugly_val v){
	op o = mkop(OP_SET_INDEX, ctx, rt, ref) ;
	o.index = i ;
	o.v = v ;
	return run(&o) ;
}


/*
 * Calls are the hot path of re-entry, and Lua limits nested C calls (about
 * 200), so a call costs one: the target is pcalled directly, not through
 * op_body. Nothing here can raise a Lua error except running out of memory.
 */
static ugly_status lua_call_op(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_kind want, ugly_val *out){

	lrt *r = ugly_rt_native(rt) ;
	lua_State *L = r->L ;
	int top = lua_gettop(L) ;
	ugly_status st = UGLY_OK ;
	lua_rawgeti(L, LUA_REGISTRYINDEX, REF(ref)) ;
	if (! callable(L, -1)){
		st = ugly_fail(ctx, rt, UGLY_ERR_NOT_CAPABLE, "TypeError", "value is not callable") ;
	}
	size_t i ;
	for (i = 0 ; (i < nargs)&&(st == UGLY_OK) ; i++){
		st = push_val(ctx, r, args[i]) ;
	}
	if (st == UGLY_OK){
		st = (lua_pcall(L, (int)nargs, 1, 0) != LUA_OK ? lua_failure(ctx, r) : to_val(ctx, r, -1, want, out)) ;
	}
	lua_settop(L, top) ;
	return st ;
}


static ugly_status lua_invoke(ugly_ctx *ctx, ugly_rt *rt, void *ref, const char *method, const ugly_val *args,
	size_t nargs, ugly_kind want, ugly_val *out){

	op o = mkop(OP_INVOKE, ctx, rt, ref) ;
	o.name = method ;
	o.args = args ;
	o.nargs = nargs ;
	o.want = want ;
	o.out = out ;
	return run(&o) ;
}


static ugly_status lua_new_instance(ugly_ctx *ctx, ugly_rt *rt, void *ref, const ugly_val *args, size_t nargs,
	ugly_handle *out){

	op o = mkop(OP_NEW, ctx, rt, ref) ;
	o.args = args ;
	o.nargs = nargs ;
	o.hout = out ;
	return run(&o) ;
}


static void lua_release(ugly_rt *rt, void *ref){
	lrt *r = ugly_rt_native(rt) ;
	luaL_unref(r->L, LUA_REGISTRYINDEX, REF(ref)) ;
}


static const ugly_plugin plugin = {
	.abi = UGLY_PLUGIN_ABI,
	.caps = UGLY_CAP_MULTI_RUNTIME | UGLY_CAP_RESTARTABLE | UGLY_CAP_EVAL,
	.name = "lua",
	.rt_new = lua_rt_new,
	.rt_delete = lua_rt_delete,
	.import = lua_import,
	.globals = lua_globals,
	.eval = lua_eval,
	.describe = lua_describe,
	.get_member = lua_get_member,
	.set_member = lua_set_member,
	.member_names = lua_member_names,
	.length = lua_length_op,
	.get_index = lua_get_index,
	.set_index = lua_set_index,
	.call = lua_call_op,
	.invoke = lua_invoke,
	.new_instance = lua_new_instance,
	.release = lua_release,
} ;


const ugly_plugin *ugly_plugin_lua(void){
	return &plugin ;
}
