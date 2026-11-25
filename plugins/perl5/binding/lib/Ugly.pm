package Ugly ;

# The Perl binding: use Ugly from a plain perl program to reach other
# languages (doc/contract.md section 8).
#
#   use Ugly ;
#   my $json = Ugly::module("python", "json") ;
#   print $json->dumps([1, 2, 3]) ;
#   print Ugly::eval("lua", "return 6 * 7") ;
#
# Values from other languages are Ugly::Proxy objects: call them ($p->(...)),
# call their methods ($p->m(...)), and use them as hashes ($p->{x}) or arrays
# ($p->[0], scalar @$p). Errors from other languages are Ugly::Error objects
# (->type, ->message, ->language, ->payload). See Ugly::is_proxy and
# Ugly::is_error.

use strict ;
use warnings ;

our $VERSION = '2.00' ;

require XSLoader ;
XSLoader::load('Ugly', $VERSION) ;

# Tear down while the interpreter is still whole: release what other languages
# hold of ours, and delete the runtimes Ugly::module created.
END { Ugly::_teardown() }

1 ;
