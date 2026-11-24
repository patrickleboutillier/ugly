use strict ;
use Test::More ;
use Ugly ;

plan(tests => 8) ;

my $lang_name = $ENV{UGLY_TEST_LANG} || 'perl5' ;
my $ctx = new Ugly::Context() ;

my $lang = Ugly::Language->init($ctx, $lang_name) ;
BAIL_OUT($ctx->get_error_msg()) if $ctx->has_error() ;
ok(defined($lang), "Language $lang_name initialized") ;

# For perl5 this embeds a second perl interpreter inside this one: control
# must come back to this interpreter afterwards.
my $rt = new Ugly::Runtime($ctx, $lang) ;
ok(! $ctx->has_error(), "$lang_name runtime created") or diag($ctx->get_error_msg()) ;
ok(defined($rt), "Runtime returned") ;
is($rt->get_language()->get_name(), $lang->get_name(), "Runtime language is $lang_name") ;

$rt->load_library($ctx, "calc") ;
ok(! $ctx->has_error(), "Library calc loaded") or diag($ctx->get_error_msg()) ;

$rt->load_library($ctx, "no_such_library") ;
ok($ctx->has_error(), "Missing library sets an error") ;
is($ctx->get_error(), Ugly::Error->RUNTIME_ERROR, "... UGLY_RUNTIME_ERROR") ;

# The calling interpreter is still the current one.
my @still_here = map { $_ * 2 } (1, 2, 3) ;
is_deeply(\@still_here, [2, 4, 6], "Calling interpreter still works") ;

$ctx->delete() ;
