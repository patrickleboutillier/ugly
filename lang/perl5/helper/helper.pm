package Ugly::perl5 ;

use strict ;
use Ugly ;
use Ugly::Value ;


sub load_library {
	my $ctx = shift ;
	my $rt = shift ;
	my $lib = shift ;

	Ugly::debug(3, "-> Ugly::perl5::load_library: $lib") ;
	eval "require $lib ;" ;
	if ($@){
		$ctx->set_error(Ugly::Error::RUNTIME_ERROR(), "Can't require '$lib': $@") ;
	}
	Ugly::debug(3, "<- Ugly::perl5::load_library") ;

	return ;
}


sub call_function {
	my $ctx = shift ;
	my $rt = shift ;
	my $rtype = shift ;
	my $func = shift ;
	my @args = @_ ;

	Ugly::debug(3, "-> Ugly::perl5::call_function: (" . Ugly::Type->to_string($rtype) . ")$func(" . 
		join(', ', map { $_->to_string() } @args) . ")") ;

	no strict 'refs' ;
	my $ret = $func->(map { $_->unwrap() } @args) ;
	if (defined($ret)){
		$ret = Ugly::Value->wrap($ret, $rtype) ;
	}

	Ugly::debug(3, "<- Ugly::perl5::call_function") ;
	return $ret ;
}


1 ;

