# line 1 "ugly/plugins/perl5/helper.pm"
# The Perl side of UGLY, defined in every perl interpreter UGLY embeds or
# attaches to (perl5.c embeds this file as the string helper_pm). The XS
# functions it calls (Ugly::Core::_call and friends) are perl5.c's.

# ------------------------------------------------------------------
# Ugly::Proxy: a value from another language, holding its handle.
# ------------------------------------------------------------------
package Ugly::Proxy ;

# The code ref &{$proxy} gives, per handle: made once (a new closure on
# every call cost more than the call), over a second proxy that holds its own
# reference, so it neither keeps $proxy alive nor dies with it.
my %code ;

use overload
	'&{}' => sub { my $s = shift ; $code{$$s} //= do { my $held = Ugly::Core::_held($s) ; sub { Ugly::Core::_call($held, @_) } } },
	'@{}' => sub { my @a ; tie @a, 'Ugly::Proxy::Array', $_[0] ; \@a },
	'%{}' => sub { my %h ; tie %h, 'Ugly::Proxy::Hash', $_[0] ; \%h },
	'==' => sub { Ugly::Core::_same($_[0], $_[1]) },
	'eq' => sub { Ugly::Core::_same($_[0], $_[1]) },
	'!=' => sub { ! Ugly::Core::_same($_[0], $_[1]) },     # spelled out: fallback would compare their strings
	'ne' => sub { ! Ugly::Core::_same($_[0], $_[1]) },
	'bool' => sub { 1 },
	'""' => sub { my $s = Ugly::Core::_str($_[0]) ; defined($s) ? $s : 'Ugly::Proxy(' . ${$_[0]} . ')' },
	fallback => 1 ;

# Every other method is the foreign value's (AUTOLOAD), so this package has as
# few subs of its own as it can: the functions behind it are in Ugly::Core.
our $AUTOLOAD ;
sub AUTOLOAD { my $s = shift ; (my $m = $AUTOLOAD) =~ s/.*:// ; Ugly::Core::_invoke($s, $m, @_) }
# A sub for a member that exists (Carp asks every argument for CARP_TRACE).
sub can {
	my ($s, $m) = @_ ;
	return UNIVERSAL::can($s, $m) unless ref($s) ;
	Ugly::Core::_has($s, $m) ? sub { Ugly::Core::_invoke(shift, $m, @_) } : undef
}
sub DESTROY { delete $code{${$_[0]}} ; Ugly::Core::_release($_[0]) }
# A new ithread gets no copy: a copy's DESTROY would release the handle this thread holds.
sub CLONE_SKIP { 1 }

# %$proxy: a foreign keyed collection's entries (a dict's keys, a table's),
# as a hash's are; for a value that isn't one, its members (an object's
# fields). A hash key is a string: one that reads as an integer also finds
# the entry under that integer (a Lua table's t[3]).
package Ugly::Proxy::Hash ;
sub TIEHASH { bless [ $_[1], undef, 0, Ugly::Core::_traits($_[1]) & 64 ], $_[0] }     # 64: UGLY_TRAIT_ENTRIES
sub _entry {
	my ($p, $k) = @_ ;
	my @v = Ugly::Core::_entry_get($p, $k) ;
	@v = Ugly::Core::_entry_get($p, 0 + $k) if ! @v && $k =~ /\A-?[0-9]+\z/ ;
	@v
}
sub _names { $_[0][3] ? Ugly::Core::_entry_keys($_[0][0]) : Ugly::Core::_keys($_[0][0]) }
sub FETCH { $_[0][3] ? (_entry($_[0][0], $_[1]))[0] : Ugly::Core::_get($_[0][0], $_[1], 0) }
sub STORE { $_[0][3] ? Ugly::Core::_entry_set($_[0][0], $_[1], $_[2]) : Ugly::Core::_set($_[0][0], $_[1], $_[2], 0) }
sub EXISTS { $_[0][3] ? scalar(my @v = _entry($_[0][0], $_[1])) : Ugly::Core::_has($_[0][0], $_[1]) }
sub DELETE {
	my ($s, $k) = @_ ;
	my $v = $s->FETCH($k) ;
	if ($s->[3]){
		Ugly::Core::_entry_delete($s->[0], $k) ;
		Ugly::Core::_entry_delete($s->[0], 0 + $k) if $k =~ /\A-?[0-9]+\z/ ;
	}
	else {
		Ugly::Core::_delete($s->[0], $k) ;
	}
	$v
}
sub CLEAR {
	my $s = $_[0] ;
	for my $k ($s->_names){
		$s->[3] ? Ugly::Core::_entry_delete($s->[0], $k) : Ugly::Core::_delete($s->[0], $k) ;
	}
}
sub FIRSTKEY { $_[0][1] = [ $_[0]->_names ] ; $_[0][2] = 0 ; $_[0][1][0] }
sub NEXTKEY { $_[0][1][ ++$_[0][2] ] }
sub SCALAR { scalar(my @k = $_[0]->_names) }

# @$proxy: the elements. Resizing goes through ugly_splice.
package Ugly::Proxy::Array ;
use Carp () ;
sub TIEARRAY { bless [ $_[1] ], $_[0] }
sub FETCH { Ugly::Core::_get($_[0][0], $_[1], 1) }
sub STORE { Ugly::Core::_set($_[0][0], $_[1], $_[2], 1) }     # one past the end appends
sub FETCHSIZE { Ugly::Core::_length($_[0][0]) }
sub STORESIZE {
	my ($s, $n) = @_ ;
	my $len = $s->FETCHSIZE ;
	if ($n < $len){ Ugly::Core::_splice($s->[0], $n, $len - $n) }
	elsif ($n > $len){ Ugly::Core::_splice($s->[0], $len, 0, (undef) x ($n - $len)) }
}
sub EXTEND { }
sub EXISTS { $_[1] < $_[0]->FETCHSIZE }
sub DELETE { my ($s, $i) = @_ ; my $v = $s->FETCH($i) ; $s->STORE($i, undef) ; $v }
sub CLEAR { Ugly::Core::_splice($_[0][0], 0, $_[0]->FETCHSIZE) }
sub PUSH { my $s = shift ; Ugly::Core::_splice($s->[0], $s->FETCHSIZE, 0, @_) ; $s->FETCHSIZE }
sub UNSHIFT { my $s = shift ; Ugly::Core::_splice($s->[0], 0, 0, @_) ; $s->FETCHSIZE }
sub POP {
	my $s = shift ;
	my $n = $s->FETCHSIZE or return undef ;
	my $v = $s->FETCH($n - 1) ;
	Ugly::Core::_splice($s->[0], $n - 1, 1) ;
	$v
}
sub SHIFT {
	my $s = shift ;
	$s->FETCHSIZE or return undef ;
	my $v = $s->FETCH(0) ;
	Ugly::Core::_splice($s->[0], 0, 1) ;
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
	Ugly::Core::_splice($s->[0], $off, $n, @_) ;
	wantarray ? @gone : $gone[-1]
}

# ------------------------------------------------------------------
# Ugly::Iterator: what Ugly::iter gives, and what another language gets
# for a Perl array. next returns the next value, or the empty list at the
# end (and again after): while (my ($x) = $it->next){ ... }. It holds a
# foreign iterator ({ of => $proxy }), or walks the indices of an array, a
# Perl one or a foreign one through its tie ({ of => $array, at => $i }).
# ------------------------------------------------------------------
package Ugly::Iterator ;
sub next {
	my $s = shift ;
	return Ugly::Core::_next($s->{of}) unless defined $s->{at} ;
	return if $s->{at} > $#{ $s->{of} } ;
	$s->{of}[ $s->{at}++ ]
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
sub CLONE_SKIP { 1 }     # as for proxies: the error would be freed twice

package Ugly::Package ;
sub name { ${$_[0]} }

# ------------------------------------------------------------------
# Ugly: the API, and the namespace import.
# ------------------------------------------------------------------
package Ugly ;
use Carp () ;

sub ELEMENTS () { 2 }        # UGLY_TRAIT_ELEMENTS
sub CALLABLE () { 4 }        # UGLY_TRAIT_CALLABLE
sub INSTANTIABLE () { 8 }    # UGLY_TRAIT_INSTANTIABLE

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

# Ugly::iter($x): an iterator over $x (contract §2), an object whose next
# returns the next value, or the empty list at the end. $x may be a foreign
# iterable (a Python generator, a JS array...), a Perl array, or already an
# iterator (an object with a next method), which is returned as is.
sub iter {
	my ($x) = @_ ;
	if (is_proxy($x)){
		my $it = Ugly::Core::_iter($x) ;
		return bless { of => $it }, 'Ugly::Iterator' if defined $it ;
		return bless { of => $x, at => 0 }, 'Ugly::Iterator' if Ugly::Core::_traits($x) & ELEMENTS ;
	}
	elsif (ref($x) && ref($x) ne 'Ugly::Package'){
		return $x if UNIVERSAL::can($x, 'next') ;
		return bless { of => $x, at => 0 }, 'Ugly::Iterator' if UNIVERSAL::isa($x, 'ARRAY') ;
	}
	Carp::croak('Ugly::iter: value is not iterable') ;
}

# The plugin's ugly_to_string, under its eval.
sub _str { "$_[0]" }

sub is_error { ref($_[0]) && UNIVERSAL::isa($_[0], 'Ugly::Error') }
sub is_proxy { ref($_[0]) && UNIVERSAL::isa($_[0], 'Ugly::Proxy') }

# Entries by key, for a key that isn't a string (a hash's are): Ugly::entry($p, $key)
# is the value or undef, Ugly::has_entry whether it is there, Ugly::set_entry and
# Ugly::delete_entry change it, and Ugly::entry_keys($p) lists the keys as they are.
sub entry { (Ugly::Core::_entry_get($_[0], $_[1]))[0] }
sub has_entry { scalar(my @v = Ugly::Core::_entry_get($_[0], $_[1])) }
sub set_entry { Ugly::Core::_entry_set($_[0], $_[1], $_[2]) }
sub delete_entry { Ugly::Core::_entry_delete($_[0], $_[1]) }
sub entry_keys { Ugly::Core::_entry_keys($_[0]) }


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
	for my $k (Ugly::Core::_keys($m)){
		next unless $k =~ /^[A-Za-z_]\w*\z/ ;
		my $v = Ugly::Core::_get($m, $k, 0) ;
		bind_package("${pkg}::$k", $v) if is_proxy($v) && (Ugly::Core::_traits($v) & INSTANTIABLE) ;
	}
	$pkg
}

my %reserved = map { ($_ => 1) } qw(AUTOLOAD DESTROY BEGIN END INIT CHECK UNITCHECK import unimport can isa DOES VERSION) ;

sub bind_package {
	my ($pkg, $target) = @_ ;
	no strict 'refs' ;
	my $instantiable = Ugly::Core::_traits($target) & INSTANTIABLE ;
	*{"${pkg}::new"} = sub { shift ; Ugly::Core::_new($target, @_) } if $instantiable ;
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
	return undef unless Ugly::Core::_has($target, $name) ;
	my $v = Ugly::Core::_get($target, $name, 0) ;
	if (is_proxy($v) && (Ugly::Core::_traits($v) & CALLABLE)){
		return sub {
			shift if @_ && defined($_[0]) && ! ref($_[0]) && $_[0] eq $pkg ;
			Ugly::Core::_invoke($target, $name, @_)
		} ;
	}
	sub { Ugly::Core::_get($target, $name, 0) }
}

$INC{'Ugly.pm'} ||= __FILE__ ;
1 ;
