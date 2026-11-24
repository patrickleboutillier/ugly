use strict ;
use Test::More ;
use Ugly ;

use Data::Dumper ;

plan(tests => 3) ;

my $lang_name = $ENV{UGLY_TEST_LANG} || 'perl5' ;
my $ctx = new Ugly::Context() ;

my $lang = Ugly::Language->init($ctx, $lang_name) ; 
die $ctx->get_error_msg() if $ctx->has_error() ;
ok(defined($lang), "Language $lang_name initialized") ;
my $name = $lang->get_name() ;
ok($lang_name eq $name, "Language name is $lang_name") ;
my $dir = $lang->get_dir() ;
ok(defined($dir), "Language directory is $dir") ;

$ctx->delete() ; 

