# The conformance fixtures (doc/contract.md section 9), in Perl.
#
# Type names the suite checks (describe) are class names: counter, list.
# The module is the package conformance; Point is the nested package
# conformance::Point. Exceptions are ConformanceError objects.

package conformance ;
use strict ;
use warnings ;
use B ;

our $finalized = 0 ;

sub echo { return $_[0] }

sub kind_of {
	my ($x) = @_ ;
	return "null" unless defined $x ;
	if (ref $x){
		return "proxy" if Ugly::is_proxy($x) ;
		return ref $x ;
	}
	my $flags = B::svref_2object(\$x)->FLAGS ;
	return "int" if $flags & B::SVf_IOK ;
	return "double" if $flags & B::SVf_NOK ;
	return "string" ;
}

sub make_counter { return counter->new() }
# Benchmarks (tests/bench.c): f(i) n times, and our own echo n times.
sub bench { my ($f, $n) = @_ ; $f->($_) for 1 .. $n ; return $n }
sub bench_self { my ($n) = @_ ; echo($_) for 1 .. $n ; return $n }
sub apply { my ($f, $x) = @_ ; return $f->($x) }
sub call_twice { my ($f, $x) = @_ ; return $f->($f->($x)) }
sub raise_error { die ConformanceError->new($_[0]) }

# Returns the exception object: ours as is, a foreign one through its payload.
sub catch_and_return {
	my ($f) = @_ ;
	my @r = eval { $f->() } ;
	return @r unless $@ ;
	return Ugly::is_error($@) ? $@->payload : $@ ;
}

sub rethrow { die $_[0] }
sub finalized { return $finalized }

# Call options (contract §11): keyword arguments arrive as trailing pairs.
sub kw { my ($a, %o) = @_ ; return join(' ', $a, $o{b} // 0, $o{c} // 0) }
sub context { return wantarray ? 'list' : defined(wantarray) ? 'scalar' : 'void' }


package conformance::Point ;

sub new { my ($class, $x, $y) = @_ ; return bless { x => $x, y => $y }, $class }
sub norm2 { my $self = shift ; return $self->{x} * $self->{x} + $self->{y} * $self->{y} }


package conformance::KwBox ;

sub new { my ($class, %o) = @_ ; return bless { value => $o{value} // 0 }, $class }
sub add { my ($self, %o) = @_ ; return $self->{value} + ($o{n} // 1) }


package counter ;

sub new { return bless { count => 0, items => list->new() }, shift }
sub inc { my $self = shift ; $self->{count}++ ; push @{ $self->{items} }, $self->{count} ; return $self->{count} }
sub DESTROY { $conformance::finalized++ }


package list ;

sub new { return bless [], shift }
sub DESTROY { $conformance::finalized++ }


package ConformanceError ;

sub new { my ($class, $message) = @_ ; return bless { message => $message }, $class }


1 ;
