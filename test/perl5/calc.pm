package calc ;
use strict ;


sub add {
	my ($a, $b) = @_ ;
	# warn "$this, $this->{a}" ;

	return $a + $b ;
}


sub subtract {
	my ($a, $b) = @_ ;

	return $a - $b ;
}


sub multiply {
	my ($a, $b) = @_ ;

	return $a * $b ;
}


sub divide {
	my ($a, $b) = @_ ;

	return $a / $b ;
}


1 ;
