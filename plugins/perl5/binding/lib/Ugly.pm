package Ugly ;

# The Perl binding: use Ugly from a plain perl program to reach other
# languages (doc/contract.md section 8).
#
#   use Ugly python => [ 'math', 'json' ], lua => { string => 'LuaString' } ;
#   print math::sqrt(2), json->loads('[1, 2]')->[1], LuaString::upper("abc") ;
#   use Ugly java => 'java.util.ArrayList' ;
#   my $l = java::util::ArrayList->new ;
#   push @$l, 3, 1, 2 ;
#
#   my $py = Ugly::module("python", "conformance") ;     # without importing
#   print Ugly::eval("lua", "return 6 * 7") ;
#
# "use Ugly LANG => MODULE" binds a module to a Perl package (os.path ->
# os::path, or the name in { MODULE => PACKAGE }): its functions are subs,
# callable as pkg::f(...) or pkg->f(...), and its classes are packages with
# new().
#
# Values from other languages are Ugly::Proxy objects: call them ($p->(...)),
# call their methods ($p->m(...)), and use them as hashes ($p->{x}, exists,
# delete, keys) or arrays ($p->[0], push, pop, shift, unshift, splice). Errors
# from other languages are Ugly::Error objects (->type, ->message,
# ->language, ->payload). See Ugly::is_proxy and Ugly::is_error.

use strict ;
use warnings ;

our $VERSION = '2.00' ;

require XSLoader ;
XSLoader::load('Ugly', $VERSION) ;

# Tear down while the interpreter is still whole: release what other languages
# hold of ours, and delete the runtimes Ugly::module created.
END { Ugly::_teardown() }

1 ;
