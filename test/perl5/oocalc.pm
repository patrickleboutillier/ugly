package oocalc ;
use strict ;


sub new {
	my $class = shift ;

	my $this = {} ;

	bless($this, $class) ;
	return $this ;
}


# Counts destroyed objects, so the tests can check that deleting an
# ugly_object releases the perl object.
our $destroyed = 0 ;


sub DESTROY {
	$destroyed++ ;
}


sub destroyed {
	return $destroyed ;
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
