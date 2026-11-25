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

# Namespace import: modules of other languages as Perl packages.
use Ugly python => [ 'math', 'json', 'conformance' ], lua => { string => 'LuaString' } ;
ok(math::sqrt(16) == 4, "use Ugly python => 'math': math::sqrt(16)") ;
ok(math->sqrt(81) == 9, "  ... and as a class method, math->sqrt(81)") ;
ok(defined(&math::sqrt), "  ... the sub is defined after its first call") ;
ok(abs(math::pi() - 3.14159265) < 1e-6, "a member that isn't callable reads as a sub: math::pi()") ;
ok(ref(math->can('floor')) eq 'CODE' && math->can('floor')->(2.5) == 2, "math->can('floor') gives a sub") ;
ok(! defined(math->can('no_such_thing')), "  ... and undef for a missing member") ;
eval { math::no_such_thing() } ;
ok($@ =~ /no member 'no_such_thing'/, "calling a missing member dies with its name") ;
ok(scalar(@{ json::loads('[1, 2, 3]') }) == 3, "json::loads returns a Python list") ;
ok(LuaString::upper("abc") eq "ABC", "use Ugly lua => { string => 'LuaString' }: LuaString::upper") ;
ok(conformance::Point->new(3, 4)->norm2 == 25, "a class becomes a package with new(): conformance::Point->new(3, 4)") ;

# Ties: arrays resize through ugly_splice, hashes delete through ugly_delete_member.
my $l = Ugly::eval("python", "[1, 2, 3]") ;
push @$l, 4, 5 ;
ok(scalar(@$l) == 5, "push on a Python list") ;
ok(pop(@$l) == 5 && shift(@$l) == 1, "pop and shift") ;
unshift @$l, 0 ;
my @gone = splice(@$l, 1, 2, 'a', 'b', 'c') ;
ok("@gone" eq "2 3" && join(',', @$l) eq '0,a,b,c,4', "unshift, then splice returns the removed elements") ;
ok(join(',', splice(@$l, -2)) eq 'c,4' && join(',', @$l) eq '0,a,b', "splice with a negative offset") ;
$#$l = 0 ;
ok(join(',', @$l) eq '0', "\$#list = 0 truncates") ;
@$l = (7, 8, 9) ;
ok(Ugly::module("python", "builtins")->len($l) == 3 && $l->[2] == 9, "list assignment: Python sees [7, 8, 9]") ;
@$l = () ;
ok(scalar(@$l) == 0, "assigning () clears") ;

my $t = Ugly::eval("lua", "return { 1, 2, 3 }") ;
push @$t, 4 ;
splice(@$t, 0, 1) ;
ok(join(',', @$t) eq '2,3,4', "push and splice on a Lua table") ;
my $a = Ugly::eval("js", "[2, 3]") ;
unshift @$a, 1 ;
ok(join(',', @$a) eq '1,2,3', "unshift on a JS array") ;

my $d = Ugly::eval("python", "{'a': 1, 'b': None}") ;
ok(exists($d->{b}) && ! defined($d->{b}), "exists on a member whose value is None") ;
ok(! exists($d->{zz}), "  ... and not on a missing one") ;
ok(delete($d->{a}) == 1 && ! exists($d->{a}), "delete returns the value and removes the key") ;
%$d = (x => 1, y => 2) ;
ok(join(',', sort keys %$d) eq 'x,y', "hash assignment clears, then stores") ;
my $obj = Ugly::eval("js", "({ k: 1 })") ;
delete $obj->{k} ;
ok(! exists($obj->{k}), "delete on a JS object") ;

print "1..$count\n" ;
exit($failures ? 1 : 0) ;
