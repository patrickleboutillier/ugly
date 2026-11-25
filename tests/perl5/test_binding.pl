# The Perl binding: a plain perl program uses Python, Lua and JS through Ugly (TAP).
use strict ;
use warnings ;
use Ugly ;

my ($count, $failures) = (0, 0) ;
sub ok {
	my ($cond, $what) = @_ ;
	$count++ ;
	$failures++ unless $cond ;
	print(($cond ? "ok" : "not ok") . " $count - $what\n") ;
}

my $py = Ugly::module("python", "conformance") ;
ok(Ugly::is_proxy($py), "Ugly::module('python', ...) gives a Python module, as a proxy") ;
my $text = "h\x{e9}llo" ;
utf8::upgrade($text) ;
ok($py->echo($text) eq $text, "a character string goes to Python and back") ;
ok($py->kind_of($text) eq "string", "  ... as a Python str") ;
ok($py->apply(sub { $_[0] * 2 }, 21) == 42, "Python calls a Perl sub") ;

my $c = $py->make_counter ;
$c->inc ;
ok($c->inc == 2, "a method call on a Python object: \$c->inc") ;
ok($c->{count} == 2, "a member of a Python object: \$c->{count}") ;
ok(@{ $c->{items} } == 2 && $c->{items}[0] == 1 && $c->{items}[1] == 2, "a Python list: \@{...} and [0]") ;
ok($py->echo($c) == $c, "a Python object round trip gives the same object (==)") ;
my $h = {} ;
ok($py->echo($h) == $h, "a Perl reference round trip gives the original") ;

eval { $py->raise_error("boom") } ;
ok(Ugly::is_error($@) && $@->type eq "ConformanceError" && $@->message eq "boom" && $@->language eq "python",
	"a Python error is an Ugly::Error, with type, message and language") ;

my $mine = bless {}, 'Mine' ;
eval { $py->apply(sub { die $mine }, 1) } ;
ok(ref($@) && $@ == $mine, "a Perl exception object crossing Python comes back as itself") ;

ok(abs(Ugly::module("python", "math")->sqrt(2) - 1.4142135623731) < 1e-9, "Python's math.sqrt from Perl") ;
ok(Ugly::module("lua", "string")->upper("abc") eq "ABC", "Lua's string.upper from Perl") ;
ok(Ugly::module("js", "conformance")->echo(41) + 1 == 42, "a JS module from Perl") ;
ok(Ugly::eval("python", "6 * 7") == 42, "Ugly::eval('python', code)") ;

# Python code using Perl through its own ugly module, reaching back into this very interpreter.
our $shared = 42 ;
ok(Ugly::eval("python", "__import__('ugly').eval('perl5', '\$main::shared')") == 42,
	"embedded Python uses ugly.eval('perl5', ...) to reach this program's globals") ;

print "1..$count\n" ;
exit($failures ? 1 : 0) ;
