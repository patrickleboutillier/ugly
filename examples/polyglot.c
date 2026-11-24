/*
 * Lua calls Python, with only the core between them.
 *
 * The C host loads both languages, takes math.sqrt from Python, and hands it
 * to a Lua function that calls it. Then the same call with -1: Python raises
 * ValueError, the error crosses Lua unchanged, and C gets Python's error.
 *
 * Built and run by `meson test` when both plugins are available.
 */
#include <stdio.h>
#include <string.h>

#include "ugly.h"


static int failed(ugly_ctx *ctx, ugly_status st, const char *what){
	if (st == UGLY_OK){
		return 0 ;
	}
	const ugly_error *e = ugly_last_error(ctx) ;
	fprintf(stderr, "%s failed: %s\n", what, e ? ugly_error_message(e) : "?") ;
	return 1 ;
}


int main(void){
	ugly_ctx *ctx ;
	ugly_lang *py, *lua ;
	ugly_rt *prt, *lrt ;
	if (ugly_ctx_new(&ctx) != UGLY_OK){
		return 1 ;
	}
	if (failed(ctx, ugly_lang_load(ctx, "python", &py), "load python")
		|| failed(ctx, ugly_lang_load(ctx, "lua", &lua), "load lua")
		|| failed(ctx, ugly_rt_new(ctx, py, &prt), "python runtime")
		|| failed(ctx, ugly_rt_new(ctx, lua, &lrt), "lua runtime")){
		return 1 ;
	}

	/* math.sqrt, from Python. */
	ugly_handle math ;
	ugly_val sqrt ;
	if (failed(ctx, ugly_rt_import(ctx, prt, "math", &math), "import math")
		|| failed(ctx, ugly_get_member(ctx, math, "sqrt", UGLY_HANDLE, &sqrt), "math.sqrt")){
		return 1 ;
	}

	/* A Lua function that calls whatever function it's given. */
	const char *code = "return function(f, x) return 'sqrt(' .. x .. ') = ' .. f(x) end" ;
	ugly_val show ;
	if (failed(ctx, ugly_rt_eval(ctx, lrt, code, strlen(code), UGLY_HANDLE, &show), "lua eval")){
		return 1 ;
	}

	/* Lua calls Python. */
	ugly_val args[] = { sqrt, ugly_int(2) } ;
	ugly_val out ;
	if (failed(ctx, ugly_call(ctx, show.as.h, args, 2, UGLY_STRING, &out), "show(sqrt, 2)")){
		return 1 ;
	}
	printf("%s\n", out.as.str.ptr) ;                   /* sqrt(2) = 1.4142135623731 */
	int good = (strncmp(out.as.str.ptr, "sqrt(2) = 1.41421", 17) == 0) ;
	ugly_val_release(ctx, &out) ;

	/* Python raises; the error crosses Lua and arrives in C as Python's. */
	args[1] = ugly_int(-1) ;
	ugly_status st = ugly_call(ctx, show.as.h, args, 2, UGLY_STRING, &out) ;
	const ugly_error *e = ugly_last_error(ctx) ;
	if ((st == UGLY_ERR_RAISED)&&(e != NULL)){
		printf("[%s] %s: %s\n", ugly_error_language(e), ugly_error_type(e), ugly_error_message(e)) ;
		good &= (strcmp(ugly_error_type(e), "ValueError") == 0) ;   /* [python] ValueError: math domain error */
	}
	else {
		good = 0 ;
	}

	ugly_val_release(ctx, &show) ;
	ugly_val_release(ctx, &sqrt) ;
	ugly_release(ctx, math) ;
	ugly_rt_delete(ctx, lrt) ;
	ugly_rt_delete(ctx, prt) ;
	ugly_lang_unload(ctx, lua) ;
	ugly_lang_unload(ctx, py) ;
	ugly_ctx_delete(ctx) ;
	return good ? 0 : 1 ;
}
