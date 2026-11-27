# The Perl binding: a plain perl program uses Python, Lua and JS through Ugly (TAP).
use strict ;
use warnings ;
use Ugly ;
use File::Basename () ;
use File::Spec () ;
use Config () ;

# Options for Python before anything starts it (the "use Ugly python => ..." below runs at compile time).
BEGIN {
	my $configured = File::Spec->rel2abs(File::Basename::dirname(__FILE__) . '/../configured') ;
	Ugly::configure(python => { path => [ $configured ] }) ;
}

my ($count, $failures) = (0, 0) ;
my %built = map { ($_ => 1) } split(/,/, $ENV{UGLY_TEST_LANGS} // '') ;
sub skip_test { my ($what, $lang) = @_ ; $count++ ; print "ok $count - $what # SKIP $lang isn't built\n" }
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
if ($built{js}){ ok(Ugly::module("js", "conformance")->echo(41) + 1 == 42, "a JS module from Perl") }
else { skip_test("a JS module from Perl", "js") }
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
if ($built{js}){
	my $a = Ugly::eval("js", "[2, 3]") ;
	unshift @$a, 1 ;
	ok(join(',', @$a) eq '1,2,3', "unshift on a JS array") ;
}
else { skip_test("unshift on a JS array", "js") }

my $d = Ugly::eval("python", "{'a': 1, 'b': None}") ;
ok(exists($d->{b}) && ! defined($d->{b}), "exists on a member whose value is None") ;
ok(! exists($d->{zz}), "  ... and not on a missing one") ;
ok(delete($d->{a}) == 1 && ! exists($d->{a}), "delete returns the value and removes the key") ;
%$d = (x => 1, y => 2) ;
ok(join(',', sort keys %$d) eq 'x,y', "hash assignment clears, then stores") ;
if ($built{js}){
	my $obj = Ugly::eval("js", "({ k: 1 })") ;
	delete $obj->{k} ;
	ok(! exists($obj->{k}), "delete on a JS object") ;
}
else { skip_test("delete on a JS object", "js") }

ok(Ugly::module("python", "ugly_configured")->{where} eq "configured", "Ugly::configure(python => { path => ... }): Python finds the module") ;
eval { Ugly::configure(python => { path => '/too/late' }) } ;
ok(Ugly::is_error($@) && $@->message =~ /already started/, "  ... and configuring it again once started dies") ;

# Keyword arguments (contract §11): Ugly::kw(...) last.
ok($py->kw(1, Ugly::kw(c => 3, b => 2)) eq "1 2 3", "keyword arguments reach Python: kw(1, Ugly::kw(c => 3, b => 2))") ;
ok(conformance::KwBox->new(Ugly::kw(value => 5))->add(Ugly::kw(n => 10)) == 15, "  ... and a class, through use Ugly: KwBox->new(Ugly::kw(value => 5))") ;
ok(json::dumps(json::loads('[3, 1]'), Ugly::kw(separators => Ugly::eval("python", "(',', ':')"))) eq "[3,1]", "json::dumps with separators") ;
eval { $py->kw(1, Ugly::opts(context => 'scalar')) } ;
ok(Ugly::is_error($@) && $@->message =~ /no call context/, "Ugly::opts(context => ...) into Python: refused") ;
ok($py->kw(1, Ugly::opts(), Ugly::kw(c => 3)) eq "1 0 3", "  ... and Ugly::opts and Ugly::kw last, in either order") ;
eval { LuaString::upper("x", Ugly::kw(a => 1)) } ;
ok(Ugly::is_error($@) && $@->message =~ /takes no call options/, "keyword arguments to Lua are refused") ;

# Iteration (contract §2): Ugly::iter, and next until the empty list.
sub drain { my $it = Ugly::iter($_[0]) ; my @all ; while (my ($x) = $it->next){ push @all, (defined $x ? $x : 'undef') } ; join(',', @all) }
ok(drain($py->count_to(3)) eq '0,1,2', "Ugly::iter over a Python generator: 0,1,2") ;
my $gen = Ugly::iter($py->count_to(2)) ;
$gen->next ;
ok($py->produced == 1, "  ... which runs only as far as asked") ;
ok(drain(Ugly::eval("python", "iter([None, 1])")) eq 'undef,1', "  ... an undef value isn't the end") ;
ok(drain(Ugly::eval("lua", "return { 5, 6 }")) eq '5,6', "a Lua table, which Lua doesn't iterate: walked by index") ;
ok(drain([7, 8]) eq '7,8', "a Perl array") ;
my $nat = Ugly::iter($py->naturals) ;
$nat->next for 1 .. 3 ;
ok(($nat->next)[0] == 3, "an infinite Python generator, stepped") ;
my $failing = Ugly::iter($py->fail_after(1)) ;
eval { $failing->next for 1 .. 2 } ;
ok(Ugly::is_error($@) && $@->type eq 'ConformanceError', "an error while iterating is an Ugly::Error") ;
eval { Ugly::iter(42) } ;
ok($@ =~ /not iterable/, "Ugly::iter(42) dies") ;

package Countdown { sub new { bless { n => $_[1] }, $_[0] } sub next { my $s = shift ; return if $s->{n} <= 0 ; $s->{n}-- } }
ok($py->sum_iter(Countdown->new(3)) == 6, "Python loops over a Perl object with a next method: 3+2+1") ;
ok($py->sum_iter([1, 2, 3]) == 6, "  ... and over a Perl array") ;
ok(Ugly::eval("python", "list(__import__('ugly').eval('perl5', 'Countdown->new(2)'))")->[1] == 1, "  ... list() over a Perl iterator") ;

# Stringification: a proxy's "" is its language's.
my $sc = $py->make_counter ;
$sc->inc ;
ok("$sc" eq "counter(1)", "\"\$proxy\" is Python's str(): counter(1)") ;
ok($py->stringify(Countdown->new(1)) =~ /^Countdown=HASH\(0x/, "Python's str() of a Perl object is Perl's own: Countdown=HASH(0x...)") ;

# A croak in Perl's API, during an operation another language asked for, is that operation's error.
{
	require Hash::Util ;
	my %locked = (a => 1) ;
	Hash::Util::lock_ref_keys(\%locked) ;
	my $store = Ugly::eval("python", "lambda d, k, v: d.__setitem__(k, v)") ;
	eval { $store->(\%locked, "b", 2) } ;
	ok($@ =~ /disallowed key/, "Python storing into a restricted hash: Perl's error, raised in Python and back here") ;
	my %open ;
	$store->(\%open, "b", 2) ;
	ok($open{b} == 2, "  ... and the next store works") ;
}

# Keys of a proxy, when listing them comes back into Perl and moves its stack.
{
	my $grow = sub { my @big = (1) x 200000 ; my @r = sub { @_ }->(@big) ; scalar(@r) } ;
	my $obj = Ugly::eval("python",
		"lambda cb: type('O', (), {'__dir__': lambda self: (cb(), ['alpha', 'beta'])[1], 'alpha': 1, 'beta': 2})()")->($grow) ;
	my @keys = sort grep { ! /^_/ } keys %$obj ;
	ok("@keys" eq "alpha beta", "keys %\$proxy while Python's __dir__ calls back into Perl: @keys") ;
}

# ithreads: a thread gets no copy of a proxy, so ending it releases nothing.
if ($Config::Config{useithreads}){
	require threads ;
	my $dict = Ugly::eval("python", "{'a': 1}") ;
	my $err = do { eval { $py->raise_error("kept") } ; $@ } ;
	threads->create(sub { 1 })->join ;
	ok(eval { $dict->{a} == 1 }, "a proxy still works after a thread started and ended") ;
	ok($err->message eq "kept", "  ... and so does an Ugly::Error") ;
}
else {
	ok(1, "ithreads # SKIP this perl has none") for 1 .. 2 ;
}

# A JS value dropped by JS's GC while it holds a Perl value that holds an open JS iterator:
# the iterator is closed (its finally runs) after the collection, not inside it.
if ($built{js}){
	my $hold = sub { my $it = Ugly::iter($_[0]) ; $it->next ; return { it => $it } } ;
	my $run = Ugly::eval("js", q{
		(hold) => {
			globalThis.closed = 0 ;
			function* gen(){ try { yield 1 ; yield 2 } finally { let junk = [] ; for (let i = 0 ; i < 1000 ; i++) junk.push({ i, s: "x" + i }) ; globalThis.closed++ } }
			let holder = hold(gen()) ;
			holder = null ;
			return 1 ;
		}}) ;
	$run->($hold) ;
	Ugly::eval("js", 'import("ugly").then(m => { m.gc() }) ; 0') ;
	ok(Ugly::eval("js", "globalThis.closed") == 1, "a JS iterator released from JS's own collection is closed afterwards") ;
}
else { skip_test("releases from a JS finalizer", "js") }

# A JS call that fails with a job pending: the job runs after it, and its calls out don't erase the error.
if ($built{js}){
	my $fails = Ugly::eval("js", '(cb) => { Promise.resolve().then(() => cb()) ; throw new RangeError("boom") }') ;
	my $ran = 0 ;
	eval { $fails->(sub { $ran++ }) } ;
	ok(Ugly::is_error($@) && $@->type eq 'RangeError' && $@->message eq 'boom' && $ran == 1,
		"a JS error survives the job that runs after it: " . (Ugly::is_error($@) ? $@->type : $@)) ;
}
else { skip_test("a JS error and a pending job", "js") }

print "1..$count\n" ;
exit($failures ? 1 : 0) ;
