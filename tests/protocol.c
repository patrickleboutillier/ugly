/* The handle protocol: traits, members, elements, invoke, new (§2). */
#include "conformance.h"


static int has_name(fixture *f, ugly_handle names, const char *want){
	int64_t n = 0, i ;
	if (ugly_length(f->ctx, names, &n) != UGLY_OK){
		return 0 ;
	}
	for (i = 0 ; i < n ; i++){
		ugly_val v ;
		if (ugly_get_index(f->ctx, names, i, UGLY_STRING, &v) == UGLY_OK){
			int match = (strcmp(sval(&v), want) == 0) ;
			ugly_val_release(f->ctx, &v) ;
			if (match){
				return 1 ;
			}
		}
	}
	return 0 ;
}


static void describe(fixture *f, ugly_handle h, uint32_t traits, const char *type, const char *what){
	uint32_t t = 0 ;
	ugly_val name ;
	ugly_status st = ugly_describe(f->ctx, h, &t, &name) ;
	if (is_status(f->ctx, st, UGLY_OK, what)){
		ok((t & traits) == traits, "  ... has the expected traits") ;
		if (type != NULL){
			is_str(sval(&name), type, "  ... type name") ;
		}
		ugly_val_release(f->ctx, &name) ;
	}
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;
	ugly_val v ;
	ugly_status st ;

	/* Modules are handles with members. */
	describe(&f, f.mod, UGLY_TRAIT_MEMBERS, "module", "describe(module)") ;
	ugly_handle names ;
	st = ugly_member_names(ctx, f.mod, &names) ;
	if (is_status(ctx, st, UGLY_OK, "member_names(module)")){
		ok(has_name(&f, names, "echo") && has_name(&f, names, "Point"), "  ... lists the fixtures") ;
		ugly_release(ctx, names) ;
	}
	st = ugly_get_member(ctx, f.mod, "no_such_thing", UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_NOT_FOUND, "missing member: UGLY_ERR_NOT_FOUND") ;

	/* An object: methods, members and an element collection. */
	ugly_handle c = make_counter(&f, f.mod) ;
	describe(&f, c, UGLY_TRAIT_MEMBERS, "counter", "describe(counter)") ;
	int i ;
	for (i = 1 ; i <= 3 ; i++){
		st = ugly_invoke(ctx, c, "inc", NULL, 0, UGLY_INT, &v) ;
		is_status(ctx, st, UGLY_OK, "invoke(counter, inc)") ;
		is_int(v.as.i, i, "  ... returns the new count") ;
	}
	st = ugly_get_member(ctx, c, "count", UGLY_INT, &v) ;
	is_status(ctx, st, UGLY_OK, "get_member(counter, count)") ;
	is_int(v.as.i, 3, "  ... is 3") ;

	ugly_val items ;
	st = ugly_get_member(ctx, c, "items", UGLY_HANDLE, &items) ;
	if (is_status(ctx, st, UGLY_OK, "get_member(counter, items)")){
		describe(&f, items.as.h, UGLY_TRAIT_ELEMENTS, "list", "describe(items)") ;
		int64_t n = 0 ;
		st = ugly_length(ctx, items.as.h, &n) ;
		is_status(ctx, st, UGLY_OK, "length(items)") ;
		is_int(n, 3, "  ... is 3") ;
		int good = 1 ;
		for (i = 0 ; i < 3 ; i++){
			good &= (ugly_get_index(ctx, items.as.h, i, UGLY_INT, &v) == UGLY_OK)&&(v.as.i == i + 1) ;
		}
		ok(good, "get_index(items, 0..2) == 1, 2, 3") ;
		st = ugly_get_index(ctx, items.as.h, 3, UGLY_ANY, &v) ;
		is_status(ctx, st, UGLY_ERR_NOT_FOUND, "get_index past the end: UGLY_ERR_NOT_FOUND") ;
		st = ugly_set_index(ctx, items.as.h, 3, ugly_str("four", 4)) ;
		is_status(ctx, st, UGLY_OK, "set_index one past the end appends") ;
		ugly_length(ctx, items.as.h, &n) ;
		is_int(n, 4, "  ... length is 4") ;

		/* splice: items is 1, 2, 3, "four" */
		ugly_val x[] = { ugly_str("x", 1) } ;
		st = ugly_splice(ctx, items.as.h, 1, 2, x, 1) ;
		if (is_status(ctx, st, UGLY_OK, "splice(items, 1, 2, \"x\") replaces two elements with one")){
			ugly_length(ctx, items.as.h, &n) ;
			is_int(n, 3, "  ... length is 3") ;
			ugly_get_index(ctx, items.as.h, 1, UGLY_STRING, &v) ;
			is_str(sval(&v), "x", "  ... items[1] is \"x\"") ;
			ugly_val_release(ctx, &v) ;
			ugly_get_index(ctx, items.as.h, 2, UGLY_STRING, &v) ;
			is_str(sval(&v), "four", "  ... items[2] is \"four\"") ;
			ugly_val_release(ctx, &v) ;
		}
		ugly_val more[] = { ugly_int(5), ugly_hval(f.mod) } ;
		st = ugly_splice(ctx, items.as.h, 3, 0, more, 2) ;
		is_status(ctx, st, UGLY_OK, "splice at the end appends") ;
		ugly_length(ctx, items.as.h, &n) ;
		is_int(n, 5, "  ... length is 5") ;
		bool same = false ;
		st = ugly_get_index(ctx, items.as.h, 4, UGLY_HANDLE, &v) ;
		ok((st == UGLY_OK)&&(v.kind == UGLY_HANDLE)&&(ugly_same(ctx, v.as.h, f.mod, &same) == UGLY_OK)&&same,
			"  ... an inserted handle reads back as the same value") ;
		ugly_val_release(ctx, &v) ;
		st = ugly_splice(ctx, items.as.h, 0, 1, NULL, 0) ;
		is_status(ctx, st, UGLY_OK, "splice(items, 0, 1) removes the first element") ;
		ugly_get_index(ctx, items.as.h, 0, UGLY_STRING, &v) ;
		is_str(sval(&v), "x", "  ... items[0] is now \"x\"") ;
		ugly_val_release(ctx, &v) ;
		st = ugly_splice(ctx, items.as.h, 2, 3, NULL, 0) ;
		is_status(ctx, st, UGLY_ERR_NOT_FOUND, "splice past the end: UGLY_ERR_NOT_FOUND") ;
		st = ugly_splice(ctx, items.as.h, 0, 4, NULL, 0) ;
		is_status(ctx, st, UGLY_OK, "splice(items, 0, length) clears") ;
		ugly_length(ctx, items.as.h, &n) ;
		is_int(n, 0, "  ... length is 0") ;
		ugly_val_release(ctx, &items) ;
	}

	st = ugly_set_member(ctx, c, "count", ugly_int(10)) ;
	is_status(ctx, st, UGLY_OK, "set_member(counter, count, 10)") ;
	ugly_get_member(ctx, c, "count", UGLY_INT, &v) ;
	is_int(v.as.i, 10, "  ... reads back as 10") ;

	/* delete_member: exact where objects take new members, refused where their members are fixed. */
	st = ugly_set_member(ctx, c, "extra", ugly_int(1)) ;
	if (st == UGLY_OK){
		st = ugly_delete_member(ctx, c, "extra") ;
		is_status(ctx, st, UGLY_OK, "delete_member(counter, extra)") ;
		st = ugly_get_member(ctx, c, "extra", UGLY_ANY, &v) ;
		is_status(ctx, st, UGLY_ERR_NOT_FOUND, "  ... it's gone") ;
		st = ugly_delete_member(ctx, c, "extra") ;
		is_status(ctx, st, UGLY_OK, "deleting a missing member is not an error") ;
	}
	else {
		st = ugly_delete_member(ctx, c, "count") ;
		is_status(ctx, st, UGLY_ERR_NOT_CAPABLE, "fixed members (no new member could be set): delete_member is UGLY_ERR_NOT_CAPABLE") ;
	}

	st = ugly_call(ctx, c, NULL, 0, UGLY_ANY, &v) ;
	is_status(ctx, st, UGLY_ERR_NOT_CAPABLE, "call(counter): UGLY_ERR_NOT_CAPABLE") ;
	ugly_release(ctx, c) ;

	/* Classes: new. */
	ugly_val cls ;
	st = ugly_get_member(ctx, f.mod, "Point", UGLY_HANDLE, &cls) ;
	if (is_status(ctx, st, UGLY_OK, "get_member(module, Point)")){
		describe(&f, cls.as.h, UGLY_TRAIT_INSTANTIABLE, NULL, "describe(Point)") ;     /* a class's type name is the language's own */
		ugly_val xy[] = { ugly_int(3), ugly_int(4) } ;
		ugly_handle p ;
		st = ugly_new(ctx, cls.as.h, xy, 2, &p) ;
		if (is_status(ctx, st, UGLY_OK, "new(Point, 3, 4)")){
			st = ugly_invoke(ctx, p, "norm2", NULL, 0, UGLY_DOUBLE, &v) ;
			is_status(ctx, st, UGLY_OK, "invoke(point, norm2)") ;
			ok(v.as.d == 25.0, "  ... is 25") ;
			st = ugly_get_member(ctx, p, "x", UGLY_DOUBLE, &v) ;
			ok((st == UGLY_OK)&&(v.as.d == 3.0), "get_member(point, x) == 3") ;
			ugly_release(ctx, p) ;
		}
		ugly_val_release(ctx, &cls) ;
	}

	/* Core lists and maps speak the same protocol. */
	ugly_val init[] = { ugly_int(1), ugly_str("a", 1) } ;
	ugly_handle l ;
	st = ugly_list_new(ctx, init, 2, &l) ;
	if (is_status(ctx, st, UGLY_OK, "ugly_list_new")){
		describe(&f, l, UGLY_TRAIT_ELEMENTS, "list", "describe(core list)") ;
		ugly_get_index(ctx, l, 1, UGLY_STRING, &v) ;
		is_str(sval(&v), "a", "get_index(core list, 1)") ;
		ugly_val_release(ctx, &v) ;
		ugly_val ins[] = { ugly_int(7), ugly_int(8) } ;
		st = ugly_splice(ctx, l, 0, 1, ins, 2) ;
		is_status(ctx, st, UGLY_OK, "splice(core list, 0, 1, 7, 8)") ;
		int64_t n = 0 ;
		ugly_length(ctx, l, &n) ;
		ugly_get_index(ctx, l, 2, UGLY_STRING, &v) ;
		ok((n == 3)&&(v.kind == UGLY_STRING)&&(strcmp(sval(&v), "a") == 0), "  ... gives 7, 8, \"a\"") ;
		ugly_val_release(ctx, &v) ;
		ugly_release(ctx, l) ;
	}
	ugly_handle m ;
	st = ugly_map_new(ctx, &m) ;
	if (is_status(ctx, st, UGLY_OK, "ugly_map_new")){
		ugly_set_member(ctx, m, "k", ugly_double(1.5)) ;
		st = ugly_get_member(ctx, m, "k", UGLY_DOUBLE, &v) ;
		ok((st == UGLY_OK)&&(v.as.d == 1.5), "set_member/get_member on a core map") ;
		st = ugly_delete_member(ctx, m, "k") ;
		is_status(ctx, st, UGLY_OK, "delete_member on a core map") ;
		st = ugly_get_member(ctx, m, "k", UGLY_ANY, &v) ;
		is_status(ctx, st, UGLY_ERR_NOT_FOUND, "  ... it's gone") ;
		ugly_release(ctx, m) ;
	}

	/*
	 * More arguments than any fixture takes, and than an interpreter's stack
	 * has room for unasked (Lua guarantees a C function 20 slots): taken or
	 * refused, by the language's own rules, and the runtime works afterwards.
	 */
	enum { MANY = 1000 } ;
	static ugly_val many[MANY] ;
	for (i = 0 ; i < MANY ; i++){
		many[i] = ugly_int(i) ;
	}
	st = fcall(&f, f.mod, "echo", many, MANY, UGLY_ANY, &v) ;
	ok((st == UGLY_OK)||(ugly_last_error(ctx) != NULL), "call with %d arguments: taken or refused", MANY) ;
	ugly_val_release(ctx, &v) ;
	c = make_counter(&f, f.mod) ;
	st = ugly_invoke(ctx, c, "inc", many, MANY, UGLY_ANY, &v) ;
	ok((st == UGLY_OK)||(ugly_last_error(ctx) != NULL), "invoke with %d arguments: taken or refused", MANY) ;
	ugly_val_release(ctx, &v) ;
	ugly_release(ctx, c) ;
	if (ugly_get_member(ctx, f.mod, "Point", UGLY_HANDLE, &cls) == UGLY_OK){
		ugly_handle p = UGLY_NO_HANDLE ;
		st = ugly_new(ctx, cls.as.h, many, MANY, &p) ;
		ok((st == UGLY_OK)||(ugly_last_error(ctx) != NULL), "new with %d arguments: taken or refused", MANY) ;
		ugly_release(ctx, p) ;
		ugly_val_release(ctx, &cls) ;
	}
	v = ugly_int(7) ;
	ugly_val back ;
	st = fcall(&f, f.mod, "echo", &v, 1, UGLY_INT, &back) ;
	ok((st == UGLY_OK)&&(back.as.i == 7), "  ... and the runtime still works") ;

	fx_teardown(&f) ;
	return done_testing() ;
}
