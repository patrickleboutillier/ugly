use strict ;
use Test::More ;
use Ugly ;

use Data::Dumper ;

plan(tests => 5) ;

my $l = Ugly::Value->new_long(42) ;
ok($l->get_long() == 42, "Long value is 42") ;
$l->delete() ;

my $d = Ugly::Value->new_double(42.6) ;
ok($d->get_double() == 42.6, "Double value is 42.6") ;
$d->delete() ;

my $s = Ugly::Value->new_string("some string") ;
ok($s->get_string() eq "some string", "String value is \"some string\"") ;
$s->delete() ;

$s = Ugly::Value->new_string("") ;
ok($s->get_string() eq "", "String value is \"\"") ;
$s->delete() ;

$s = Ugly::Value->new_string(undef) ;
ok(! defined($s->get_string()), "String value is undef") ;
$s->delete() ;
