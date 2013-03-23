package oocalc ;
use strict ;


sub new {
	my $class = shift ;

	my $this = {} ;

	bless($this, $class) ;
	warn "$class" ;
	return $this ;
}


sub DESTROY {
	warn "DESTROY!" ;
}


sub add {
	my $this = shift ;
	my ($a, $b) = @_ ;

	return $a + $b ;
}


sub subtract {
	my $this = shift ;
	my ($a, $b) = @_ ;

	return $a - $b ;
}


sub multiply {
	my $this = shift ;
	my ($a, $b) = @_ ;

	return $a * $b ;
}


sub divide {
	my $this = shift ;
	my ($a, $b) = @_ ;

	return $a / $b ;
}


1 ;
