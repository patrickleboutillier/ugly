use strict ;
use Test::More ;
use Ugly ;

use Data::Dumper ;

plan(tests => 9) ;

my $ctx = new Ugly::Context() ;
ok(!$ctx->has_error(), "Context doesn't have a pending error") ;
ok($ctx->get_error() == Ugly::Error->OK, "Context error is Ugly_OK") ;
ok(! defined($ctx->get_error_msg()), "Context error msg is NULL") ;

$ctx->set_error(Ugly::Error->INTERNAL_ERROR, undef) ;
ok($ctx->get_error() == Ugly::Error->INTERNAL_ERROR, "Context error is Ugly_INTERNAL_ERROR") ;
is($ctx->get_error_msg(), undef, "Context error msg is NULL") ;

$ctx->clear_error() ;
ok($ctx->get_error() == Ugly::Error->OK, "Context error is Ugly_OK") ;
ok(! defined($ctx->get_error_msg()), "Context error msg is NULL") ;

$ctx->set_error(Ugly::Error->INTERNAL_ERROR, "internal error") ;
ok($ctx->get_error() == Ugly::Error->INTERNAL_ERROR, "Context error is Ugly_INTERNAL_ERROR") ;
ok($ctx->get_error_msg() eq "internal error", "Context error msg is \"internal error\"") ;

$ctx->delete() ;
