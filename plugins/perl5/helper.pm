# line 1 "ugly/plugins/perl5/helper.pm"
# The Perl side of UGLY, defined in every perl interpreter UGLY embeds or
# attaches to (perl5.c embeds this file as the string helper_pm). The XS
# functions it calls (Ugly::Proxy::_call and friends) are perl5.c's.

# ------------------------------------------------------------------
# Ugly::Proxy: a value from another language, holding its handle.
# ------------------------------------------------------------------
package Ugly::Proxy ;

# The code ref &{$proxy} gives, per handle: made once (a new closure on
# every call cost more than the call), over a second proxy that holds its own
# reference, so it neither keeps $proxy alive nor dies with it.
my %code ;

use overload
	'&{}' => sub { my $s = shift ; $code{$$s} //= do { my $held = Ugly::Proxy::_held($s) ; sub { Ugly::Proxy::_call($held, @_) } } },
	'@{}' => sub { my @a ; tie @a, 'Ugly::Proxy::Array', $_[0] ; \@a },
	'%{}' => sub { my %h ; tie %h, 'Ugly::Proxy::Hash', $_[0] ; \%h },
	'==' => sub { Ugly::Proxy::_same($_[0], $_[1]) },
	'eq' => sub { Ugly::Proxy::_same($_[0], $_[1]) },
	'bool' => sub { 1 },
	'""' => sub { 'Ugly::Proxy(' . ${$_[0]} . ')' },
	fallback => 1 ;

our $AUTOLOAD ;
sub AUTOLOAD { my $s = shift ; (my $m = $AUTOLOAD) =~ s/.*:// ; Ugly::Proxy::_invoke($s, $m, @_) }
sub can { my ($s, $m) = @_ ; ref($s) ? sub { Ugly::Proxy::_invoke(shift, $m, @_) } : UNIVERSAL::can($s, $m) }
sub DESTROY { delete $code{${$_[0]}} ; Ugly::Proxy::_release($_[0]) }

# %$proxy: the members (fields, methods, dict/hash/table entries).
package Ugly::Proxy::Hash ;
sub TIEHASH { bless [ $_[1], undef, 0 ], $_[0] }
sub FETCH { Ugly::Proxy::_get($_[0][0], $_[1], 0) }
sub STORE { Ugly::Proxy::_set($_[0][0], $_[1], $_[2], 0) }
sub EXISTS { Ugly::Proxy::_has($_[0][0], $_[1]) }
sub DELETE {
	my ($s, $k) = @_ ;
	my $v = Ugly::Proxy::_get($s->[0], $k, 0) ;
	Ugly::Proxy::_delete($s->[0], $k) ;
	$v
}
sub CLEAR { my $p = $_[0][0] ; Ugly::Proxy::_delete($p, $_) for Ugly::Proxy::_keys($p) }
sub FIRSTKEY { $_[0][1] = [ Ugly::Proxy::_keys($_[0][0]) ] ; $_[0][2] = 0 ; $_[0][1][0] }
sub NEXTKEY { $_[0][1][ ++$_[0][2] ] }
sub SCALAR { scalar(my @k = Ugly::Proxy::_keys($_[0][0])) }

# @$proxy: the elements. Resizing goes through ugly_splice.
package Ugly::Proxy::Array ;
use Carp () ;
sub TIEARRAY { bless [ $_[1] ], $_[0] }
sub FETCH { Ugly::Proxy::_get($_[0][0], $_[1], 1) }
sub STORE { Ugly::Proxy::_set($_[0][0], $_[1], $_[2], 1) }     # one past the end appends
sub FETCHSIZE { Ugly::Proxy::_length($_[0][0]) }
sub STORESIZE {
	my ($s, $n) = @_ ;
	my $len = $s->FETCHSIZE ;
	if ($n < $len){ Ugly::Proxy::_splice($s->[0], $n, $len - $n) }
	elsif ($n > $len){ Ugly::Proxy::_splice($s->[0], $len, 0, (undef) x ($n - $len)) }
}
sub EXTEND { }
sub EXISTS { $_[1] < $_[0]->FETCHSIZE }
sub DELETE { my ($s, $i) = @_ ; my $v = $s->FETCH($i) ; $s->STORE($i, undef) ; $v }
sub CLEAR { Ugly::Proxy::_splice($_[0][0], 0, $_[0]->FETCHSIZE) }
sub PUSH { my $s = shift ; Ugly::Proxy::_splice($s->[0], $s->FETCHSIZE, 0, @_) ; $s->FETCHSIZE }
sub UNSHIFT { my $s = shift ; Ugly::Proxy::_splice($s->[0], 0, 0, @_) ; $s->FETCHSIZE }
sub POP {
	my $s = shift ;
	my $n = $s->FETCHSIZE or return undef ;
	my $v = $s->FETCH($n - 1) ;
	Ugly::Proxy::_splice($s->[0], $n - 1, 1) ;
	$v
}
sub SHIFT {
	my $s = shift ;
	$s->FETCHSIZE or return undef ;
	my $v = $s->FETCH(0) ;
	Ugly::Proxy::_splice($s->[0], 0, 1) ;
	$v
}
# Perl's own rules: a negative offset counts from the end, a missing length
# means to the end, a negative one leaves that many at the end.
sub SPLICE {
	my $s = shift ;
	my $len = $s->FETCHSIZE ;
	my $off = @_ ? shift : 0 ;
	$off += $len if $off < 0 ;
	Carp::croak('Modification of non-creatable array value attempted') if $off < 0 ;
	$off = $len if $off > $len ;
	my $n = @_ ? shift : $len - $off ;
	$n = $len - $off + $n if $n < 0 ;
	$n = 0 if $n < 0 ;
	$n = $len - $off if $n > $len - $off ;
	my @gone = map { $s->FETCH($_) } $off .. $off + $n - 1 ;
	Ugly::Proxy::_splice($s->[0], $off, $n, @_) ;
	wantarray ? @gone : $gone[-1]
}

# ------------------------------------------------------------------
# Ugly::Error: an error from another language.
# ------------------------------------------------------------------
package Ugly::Error ;
use overload '""' => sub { "[$_[0]{language}] $_[0]{type}: $_[0]{message}" }, fallback => 1 ;
sub type { $_[0]{type} }
sub message { $_[0]{message} }
sub language { $_[0]{language} }
sub payload { $_[0]{payload} }
sub DESTROY { Ugly::Error::_free($_[0]) }

package Ugly::Package ;
sub name { ${$_[0]} }

# ------------------------------------------------------------------
# Ugly: the API, and the namespace import.
# ------------------------------------------------------------------
package Ugly ;
use Carp () ;

# Ugly::configure(java => { classpath => [ ... ], options => '-Xmx512m' }, python => { path => './py' }) ;
# Options for languages that haven't started yet (in a BEGIN block, before
# "use Ugly LANG => ..." starts them): strings or lists of strings.
sub configure {
	Carp::croak('Ugly::configure(LANG => { OPTION => VALUE, ... }, ...): expected pairs') if @_ % 2 ;
	while (@_){
		my ($lang, $opts) = splice(@_, 0, 2) ;
		Carp::croak("Ugly::configure: the options for $lang are a hash") unless ref($opts) eq 'HASH' ;
		for my $key (sort keys %$opts){
			my $v = $opts->{$key} ;
			Ugly::_configure($lang, $key, $_) for (ref($v) eq 'ARRAY' ? @$v : $v) ;
		}
	}
}

# Ugly::kw(indent => 2, ...): keyword arguments, for the last argument of a
# call into another language (contract §11): json::dumps($data, Ugly::kw(indent => 2)).
sub kw {
	Carp::croak('Ugly::kw(name => value, ...): expected pairs') if @_ % 2 ;
	bless [ @_ ], 'Ugly::Kw'
}

# Ugly::opts(context => 'scalar', hint => '...'): call options, for the last
# argument of a call (with an Ugly::kw(...) beside it if there are both).
sub opts {
	Carp::croak('Ugly::opts(hint => ..., context => ...): expected pairs') if @_ % 2 ;
	my %o = @_ ;
	for (keys %o){
		Carp::croak("Ugly::opts: no option '$_' (hint, context)") unless $_ eq 'hint' || $_ eq 'context' ;
	}
	bless { %o }, 'Ugly::Opts'
}

sub is_error { ref($_[0]) && UNIVERSAL::isa($_[0], 'Ugly::Error') }
sub is_proxy { ref($_[0]) && UNIVERSAL::isa($_[0], 'Ugly::Proxy') }

sub CALLABLE () { 4 }        # UGLY_TRAIT_CALLABLE
sub INSTANTIABLE () { 8 }    # UGLY_TRAIT_INSTANTIABLE

# use Ugly python => 'json' ;                         # json::dumps(...), json->dumps(...)
# use Ugly python => [ 'math', 'os.path' ] ;          # math::sqrt, os::path::join
# use Ugly java => { 'java.util.ArrayList' => 'ArrayList' } ;   # ArrayList->new
#
# A module of another language becomes a Perl package: its functions are
# subs (defined on first use), its other members read as subs, its classes
# become nested packages with new(). A class imported itself gets new() too.
sub import {
	my $class = shift ;
	Carp::croak('use Ugly LANG => MODULE, ...: expected pairs') if @_ % 2 ;
	while (@_){
		my ($lang, $what) = splice(@_, 0, 2) ;
		my @pairs = (ref($what) eq 'HASH') ? %$what
			: map { ($_ => undef) } (ref($what) eq 'ARRAY' ? @$what : $what) ;
		while (@pairs){
			my ($name, $pkg) = splice(@pairs, 0, 2) ;
			bind_module($lang, $name, $pkg) ;
		}
	}
}

# The package for a module name: os.path and java.util.List map to os::path
# and java::util::List.
sub package_for {
	my ($name) = @_ ;
	(my $pkg = $name) =~ s{[./]}{::}g ;
	Carp::croak("Ugly: '$name' doesn't make a Perl package name; name one: use Ugly LANG => { '$name' => 'Name' }")
		unless $pkg =~ /^[A-Za-z_]\w*(?:::\w+)*\z/ ;
	$pkg
}

# Binds module 'name' of 'lang' to package 'pkg' (default: package_for(name)); returns the package.
sub bind_module {
	my ($lang, $name, $pkg) = @_ ;
	$pkg = package_for($name) unless defined $pkg ;
	my $m = Ugly::module($lang, $name) ;
	bind_package($pkg, $m) ;
	for my $k (Ugly::Proxy::_keys($m)){
		next unless $k =~ /^[A-Za-z_]\w*\z/ ;
		my $v = Ugly::Proxy::_get($m, $k, 0) ;
		bind_package("${pkg}::$k", $v) if is_proxy($v) && (Ugly::Proxy::_traits($v) & INSTANTIABLE) ;
	}
	$pkg
}

my %reserved = map { ($_ => 1) } qw(AUTOLOAD DESTROY BEGIN END INIT CHECK UNITCHECK import unimport can isa DOES VERSION) ;

sub bind_package {
	my ($pkg, $target) = @_ ;
	no strict 'refs' ;
	my $instantiable = Ugly::Proxy::_traits($target) & INSTANTIABLE ;
	*{"${pkg}::new"} = sub { shift ; Ugly::Proxy::_new($target, @_) } if $instantiable ;
	*{"${pkg}::AUTOLOAD"} = sub {
		# Perl sets $AUTOLOAD in the package this closure was compiled in: ours.
		(my $name = $Ugly::AUTOLOAD) =~ s/.*:// ;
		return if $name eq 'DESTROY' ;
		my $sub = member_sub($pkg, $target, $name)
			or Carp::croak("Undefined subroutine &${pkg}::$name called (no member '$name')") ;
		*{"${pkg}::$name"} = $sub unless $reserved{$name} ;
		goto &$sub ;
	} ;
	*{"${pkg}::can"} = sub {
		my ($self, $name) = @_ ;
		UNIVERSAL::can($self, $name) || member_sub($pkg, $target, $name)
	} ;
	$pkg
}

# A sub for member 'name': a call (invoke) if the member is callable, else a
# read of its current value. Called as pkg::f(...) or pkg->f(...), so a
# leading class name is dropped.
sub member_sub {
	my ($pkg, $target, $name) = @_ ;
	return undef unless Ugly::Proxy::_has($target, $name) ;
	my $v = Ugly::Proxy::_get($target, $name, 0) ;
	if (is_proxy($v) && (Ugly::Proxy::_traits($v) & CALLABLE)){
		return sub {
			shift if @_ && defined($_[0]) && ! ref($_[0]) && $_[0] eq $pkg ;
			Ugly::Proxy::_invoke($target, $name, @_)
		} ;
	}
	sub { Ugly::Proxy::_get($target, $name, 0) }
}

$INC{'Ugly.pm'} ||= __FILE__ ;
1 ;
