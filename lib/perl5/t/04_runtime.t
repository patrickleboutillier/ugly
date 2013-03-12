use strict ;
use Test::More ;
use Ugly ;

use Data::Dumper ;

plan(tests => 3) ;

my $ctx = new Ugly::Context() ;

my $lang = Ugly::Language->init($ctx, $ENV{UGLY_TEST_LANG}) ; 
die $ctx->get_error_msg() if $ctx->has_error() ;
ok(defined($lang), "Language $ENV{UGLY_TEST_LANG} initialized") ;

warn "$ctx" ;
warn "$lang" ;

my $rt = new Ugly::Runtime($ctx, $lang) ;
warn "$ctx" ;
die $ctx->get_error_msg() if $ctx->has_error() ;
#ok(defined($rt), "$ENV{UGLY_TEST_LANG} runtime initialized") ;
#ok($rt->get_language()->get_name() eq $lang->get_name(), "Runtime language == language") ;

warn "$lang" ;
warn "$rt" ;

$ctx->delete() ; 

