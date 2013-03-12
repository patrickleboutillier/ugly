use strict ;
use Test::More ;
use Ugly ;

use Data::Dumper ;

plan(tests => 3) ;

my $ctx = new Ugly::Context() ;

my $lang = Ugly::Language->init($ctx, $ENV{UGLY_TEST_LANG}) ; 
die $ctx->get_error_msg() if $ctx->has_error() ;
ok(defined($lang), "Language $ENV{UGLY_TEST_LANG} initialized") ;
my $name = $lang->get_name() ;
ok($ENV{UGLY_TEST_LANG} eq $name, "Language name is $ENV{UGLY_TEST_LANG}") ;
my $dir = $lang->get_dir() ;
ok(defined($dir), "Language directory is $dir") ;

$ctx->delete() ; 

