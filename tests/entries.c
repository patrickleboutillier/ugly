/*
 * Entries (contract §2, libugly 2.8): a keyed collection's, by key, apart
 * from its members. Exact or refused.
 *
 * Fixtures: make_table() is the language's dictionary holding
 * { "one": 1, "two": 2, 3: "three" } (the last under "3" where keys are
 * strings); table_method() names a method every such table has, which an
 * entry may be named too, or is null where tables have none. A plugin
 * without entries yet gets a SKIP.
 */
#include "conformance.h"


/* Runs a keys iterator to its end: how many keys, and whether the string 'want' was one. */
static int64_t keys_of(ugly_ctx *ctx, ugly_handle table, const char *want, int *found){
	ugly_handle it = UGLY_NO_HANDLE ;
	*found = 0 ;
	if (ugly_entry_keys(ctx, table, &it) != UGLY_OK){
		return -1 ;
	}
	int64_t n = 0 ;
	for (;;){
		ugly_val k[4] ;
		size_t got = 0, i ;
		if (ugly_next(ctx, it, UGLY_ANY, k, 4, &got) != UGLY_OK){
			n = -1 ;
			break ;
		}
		for (i = 0 ; i < got ; i++){
			if ((k[i].kind == UGLY_STRING)&&(strcmp(sval(&k[i]), want) == 0)){
				*found = 1 ;
			}
			ugly_val_release(ctx, &k[i]) ;
		}
		n += (int64_t)got ;
		if (got < 4){
			break ;
		}
	}
	ugly_release(ctx, it) ;
	return n ;
}


static void core_maps(ugly_ctx *ctx){
	/* A core map is a record and a dictionary at once: its entries are its members, by string keys. */
	ugly_handle m = UGLY_NO_HANDLE ;
	ugly_val v ;
	uint32_t traits = 0 ;
	ugly_map_new(ctx, &m) ;
	if (ugly_describe(ctx, m, &traits, &v) == UGLY_OK){
		ugly_val_release(ctx, &v) ;
	}
	ok((traits & UGLY_TRAIT_ENTRIES)&&(traits & UGLY_TRAIT_MEMBERS), "a core map has ENTRIES and MEMBERS") ;
	is_status(ctx, ugly_set_entry(ctx, m, ugly_str("k", 1), ugly_int(5)), UGLY_OK, "set_entry on a core map") ;
	ugly_status st = ugly_get_member(ctx, m, "k", UGLY_INT, &v) ;
	ok((st == UGLY_OK)&&(v.as.i == 5), "  ... is the member of that name") ;
	st = ugly_get_entry(ctx, m, ugly_str("k", 1), UGLY_INT, &v) ;
	ok((st == UGLY_OK)&&(v.as.i == 5), "  ... and get_entry reads it") ;
	is_status(ctx, ugly_set_entry(ctx, m, ugly_int(1), ugly_int(5)), UGLY_ERR_TYPE, "a core map's keys are strings: another to set is UGLY_ERR_TYPE") ;
	is_status(ctx, ugly_get_entry(ctx, m, ugly_int(1), UGLY_ANY, &v), UGLY_ERR_NOT_FOUND, "  ... and isn't there to get: UGLY_ERR_NOT_FOUND") ;
	int64_t n = -1 ;
	int found = 0 ;
	ok((ugly_entry_count(ctx, m, &n) == UGLY_OK)&&(n == 1)&&(keys_of(ctx, m, "k", &found) == 1)&& found,
		"entry_count and entry_keys on a core map") ;
	is_status(ctx, ugly_delete_entry(ctx, m, ugly_str("k", 1)), UGLY_OK, "delete_entry on a core map") ;
	is_status(ctx, ugly_get_entry(ctx, m, ugly_str("k", 1), UGLY_ANY, &v), UGLY_ERR_NOT_FOUND, "  ... it's gone") ;
	ugly_release(ctx, m) ;

	ugly_handle l = UGLY_NO_HANDLE ;
	ugly_list_new(ctx, NULL, 0, &l) ;
	is_status(ctx, ugly_get_entry(ctx, l, ugly_int(0), UGLY_ANY, &v), UGLY_ERR_NOT_CAPABLE, "a core list has no entries: UGLY_ERR_NOT_CAPABLE") ;
	is_status(ctx, ugly_entry_count(ctx, l, NULL), UGLY_ERR_USAGE, "entry_count without out: UGLY_ERR_USAGE") ;
	ugly_release(ctx, l) ;
}


int main(void){
	fixture f ;
	fx_setup(&f) ;
	ugly_ctx *ctx = f.ctx ;
	ugly_val v, t ;
	ugly_status st ;
	int64_t n = -1 ;
	int found = 0 ;

	core_maps(ctx) ;

	st = fcall(&f, f.mod, "make_table", NULL, 0, UGLY_HANDLE, &t) ;
	if ((st != UGLY_OK)||(ugly_entry_count(ctx, t.as.h, &n) == UGLY_ERR_NOT_IMPLEMENTED)){
		if (st == UGLY_OK){
			ugly_val_release(ctx, &t) ;
		}
		skip("plugin has no entries yet: a table's entries") ;
		fx_teardown(&f) ;
		return done_testing() ;
	}
	ugly_handle table = t.as.h ;

	uint32_t traits = 0 ;
	if (ugly_describe(ctx, table, &traits, &v) == UGLY_OK){
		ugly_val_release(ctx, &v) ;
	}
	ok((traits & UGLY_TRAIT_ENTRIES) != 0, "a table is described as ENTRIES") ;
	is_int(n, 3, "entry_count: 3") ;
	st = ugly_get_entry(ctx, table, ugly_str("one", 3), UGLY_INT, &v) ;
	ok((st == UGLY_OK)&&(v.as.i == 1), "get_entry(\"one\") is 1") ;
	is_status(ctx, ugly_get_entry(ctx, table, ugly_str("missing", 7), UGLY_ANY, &v), UGLY_ERR_NOT_FOUND,
		"get_entry of a key that isn't there: UGLY_ERR_NOT_FOUND") ;

	/* A key that isn't a string: found, in a language whose keys are any value or that reads 3 as "3"; else not there. */
	st = ugly_get_entry(ctx, table, ugly_int(3), UGLY_STRING, &v) ;
	ok(((st == UGLY_OK)&&(strcmp(sval(&v), "three") == 0))||(st == UGLY_ERR_NOT_FOUND),
		"get_entry(3): \"three\", or not there where keys are strings only (%s)", status_name(st)) ;
	ugly_val_release(ctx, &v) ;

	ok((keys_of(ctx, table, "two", &found) == 3)&& found, "entry_keys: an iterator over the 3 keys") ;

	/* Writing: exact, or refused as a whole where the language's collections don't change. */
	st = ugly_set_entry(ctx, table, ugly_str("four", 4), ugly_int(4)) ;
	if (st == UGLY_ERR_NOT_CAPABLE){
		skip("the language's tables can't change: set_entry") ;
		skip("the language's tables can't change: delete_entry") ;
		is_status(ctx, ugly_delete_entry(ctx, table, ugly_str("one", 3)), UGLY_ERR_NOT_CAPABLE, "  ... delete_entry is refused too") ;
	}
	else {
		is_status(ctx, st, UGLY_OK, "set_entry(\"four\", 4)") ;
		ugly_set_entry(ctx, table, ugly_str("one", 3), ugly_int(10)) ;
		st = ugly_get_entry(ctx, table, ugly_str("one", 3), UGLY_INT, &v) ;
		ok((st == UGLY_OK)&&(v.as.i == 10)&&(ugly_entry_count(ctx, table, &n) == UGLY_OK)&&(n == 4),
			"  ... and of a key that is there replaces its value: 4 entries") ;
		is_status(ctx, ugly_delete_entry(ctx, table, ugly_str("two", 3)), UGLY_OK, "delete_entry(\"two\")") ;
		is_status(ctx, ugly_get_entry(ctx, table, ugly_str("two", 3), UGLY_ANY, &v), UGLY_ERR_NOT_FOUND, "  ... it's gone") ;
		is_status(ctx, ugly_delete_entry(ctx, table, ugly_str("two", 3)), UGLY_OK, "  ... and deleting it again is not an error") ;

		/* An object as a key: the same object finds it, or the language's keys can't be objects. */
		ugly_handle c = make_counter(&f, f.mod) ;
		st = ugly_set_entry(ctx, table, ugly_hval(c), ugly_int(7)) ;
		if (st == UGLY_OK){
			st = ugly_get_entry(ctx, table, ugly_hval(c), UGLY_INT, &v) ;
			ok((st == UGLY_OK)&&(v.as.i == 7), "an object as a key: the same object finds its entry") ;
			ugly_delete_entry(ctx, table, ugly_hval(c)) ;
		}
		else {
			is_status(ctx, st, UGLY_ERR_TYPE, "an object as a key, where keys can't be objects: UGLY_ERR_TYPE") ;
		}
		ugly_release(ctx, c) ;

		/* Entries are apart from members: an entry named like a method doesn't hide it, nor the other way. */
		ugly_val name ;
		st = fcall(&f, f.mod, "table_method", NULL, 0, UGLY_ANY, &name) ;
		if ((st == UGLY_OK)&&(name.kind == UGLY_STRING)){
			const char *m = sval(&name) ;
			ugly_set_entry(ctx, table, ugly_str(m, strlen(m)), ugly_int(99)) ;
			st = ugly_get_entry(ctx, table, ugly_str(m, strlen(m)), UGLY_INT, &v) ;
			ok((st == UGLY_OK)&&(v.as.i == 99), "an entry named like the table's method \"%s\" is an entry", m) ;
			st = ugly_get_member(ctx, table, m, UGLY_ANY, &v) ;
			ok(! ((st == UGLY_OK)&&(v.kind == UGLY_INT)&&(v.as.i == 99)), "  ... get_member(\"%s\") doesn't reach it", m) ;
			if (st == UGLY_OK){
				ugly_val_release(ctx, &v) ;
			}
			st = ugly_invoke(ctx, table, m, NULL, 0, UGLY_ANY, &v) ;
			is_status(ctx, st, UGLY_OK, "  ... and the method is still the method") ;
			if (st == UGLY_OK){
				ugly_val_release(ctx, &v) ;
			}
		}
		else {
			skip("the language's tables have no methods: an entry named like a method") ;
			skip("the language's tables have no methods: get_member") ;
			skip("the language's tables have no methods: invoke") ;
		}
		if (st == UGLY_OK){
			ugly_val_release(ctx, &name) ;
		}
	}

	/* A value that isn't a keyed collection has no entries. In Lua an object is a table: there, it has. */
	ugly_handle c = make_counter(&f, f.mod) ;
	traits = 0 ;
	if (ugly_describe(ctx, c, &traits, &v) == UGLY_OK){
		ugly_val_release(ctx, &v) ;
	}
	st = ugly_get_entry(ctx, c, ugly_str("count", 5), UGLY_ANY, &v) ;
	if (traits & UGLY_TRAIT_ENTRIES){
		is_status(ctx, st, UGLY_OK, "an object that is a table in its language: its members are entries too") ;
	}
	else {
		ok((st == UGLY_ERR_NOT_CAPABLE)||(st == UGLY_ERR_NOT_FOUND), "an object that isn't a table: its members aren't entries (%s)",
			status_name(st)) ;
	}
	if (st == UGLY_OK){
		ugly_val_release(ctx, &v) ;
	}
	ugly_release(ctx, c) ;

	ugly_release(ctx, table) ;
	fx_teardown(&f) ;
	return done_testing() ;
}
