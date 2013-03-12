package Ugly::Value ;

use strict ;
use Carp ;


sub unwrap {
	my $this = shift ;

	my $type = $this->get_type() ;
	if ($type == Ugly::Type::BOOL()){
		return $this->get_bool() ;
	}
	elsif ($type == Ugly::Type::LONG()){
		return $this->get_long() ;
	}
	elsif ($type == Ugly::Type::DOUBLE()){
		return $this->get_double() ;
	}
	elsif ($type == Ugly::Type::CHAR()){
		return $this->get_char() ;
	}
	elsif ($type == Ugly::Type::STRING()){
		return $this->get_string() ;
	}
	else {
		croak("Unknown Ugly::Type '$type'") ;
	}
}


sub wrap {
	my $pkg = shift ;
	my $v = shift ;
	my $type = shift ;

	if ($type == Ugly::Type::BOOL()){
		return Ugly::Value->new_bool($v) ;
	}
	elsif ($type == Ugly::Type::LONG()){
		return Ugly::Value->new_long($v) ;
	}
	elsif ($type == Ugly::Type::DOUBLE()){
		return Ugly::Value->new_double($v) ;
	}
	elsif ($type == Ugly::Type::CHAR()){
		return Ugly::Value->new_char($v) ;
	}
	elsif ($type == Ugly::Type::STRING()){
		return Ugly::Value->new_string($v) ;
	}
	else {
		croak("Unknown Ugly::Type '$type'") ;
	}
}


1 ;
