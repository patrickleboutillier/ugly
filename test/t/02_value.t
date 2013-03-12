plan_tests(4) ;

{
ugly_value *l = ugly_value_new_long(42L) ;
ok(ugly_value_get_long(l) == 42L, "Long value is 42") ;
ugly_value_delete(l) ;
}

{
ugly_value *d = ugly_value_new_double(42.6) ;
ok(ugly_value_get_double(d) == 42.6, "Double value is 42.6") ;
ugly_value_delete(d) ;
}

{
ugly_value *s = ugly_value_new_string("some string") ;
ok(strcmp(ugly_value_get_string(s), "some string") == 0, "String value is \\"some string\\"") ;
ugly_value_delete(s) ;
}

{
ugly_value *s = ugly_value_new_string((const char *)NULL) ;
ok(ugly_value_get_string(s) == NULL, "String value as string is NULL") ;
ugly_value_delete(s) ;
}

