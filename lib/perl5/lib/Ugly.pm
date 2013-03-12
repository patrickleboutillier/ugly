package Ugly;

use 5.016002;
use strict;
use warnings;

require Exporter;

our @ISA = qw(Exporter);

# Items to export into callers namespace by default. Note: do not export
# names by default without a very good reason. Use EXPORT_OK instead.
# Do not simply export all your public functions/methods/constants.

# This allows declaration	use Ugly ':all';
# If you do not need this, moving things directly into @EXPORT or @EXPORT_OK
# will save memory.
our %EXPORT_TAGS = ( 'all' => [ qw(
	
) ] );

our @EXPORT_OK = ( @{ $EXPORT_TAGS{'all'} } );

our @EXPORT = qw(
	
);

our $VERSION = '0.01';

require XSLoader;
XSLoader::load('Ugly', $VERSION);



package Ugly::Error ;


use constant {
    NOT_SUPPORTED => -1000,     # Language does not support this feature
    NOT_IMPLEMENTED => -1001,   # Language supports this feature but plugin doesn't implement it yet...

    OK => 0,
    INTERNAL_ERROR => 1,
    RUNTIME_ERROR => 2,
    LIBRARY_ERROR => 3,
} ;



package Ugly::Type ;

use constant {
    BOOL => 0,
    LONG => 1,
    DOUBLE => 2,
    CHAR => 3,
    STRING => 4,
} ;


sub to_string {
	my $pkg = shift ;
    my $type = shift ;

    if ($type == BOOL()){
        return "BOOL" ;
    }
    elsif ($type == LONG()){
        return "LONG" ;
    }
    elsif ($type == DOUBLE()){
        return "DOUBLE" ;
    }
    elsif ($type == CHAR()){
        return "CHAR" ;
    }
    elsif ($type == STRING()){
        return "STRING" ;
    }
    else {
        croak("Unknown Ugly::Type '$type'") ;
    }
}



package Ugly ;


sub debug_level {
	if ($ENV{"UGLY_DEBUG"}){
		return $ENV{"UGLY_DEBUG"} + 0 ;
	}

    return 0 ;
}


sub debug_on {
	my $level = shift ;

    return ($level <= debug_level()) ;
}


sub debug {
	my $level = shift ;
	my $msg = shift ;

	my @args = ("# DEBUG(%-8.8s): %*s%s\n", "perl5", $level, "", $msg) ;
	if (debug_on($level)){
		if ($ENV{"UGLY_TEST_UNDER_HARNESS"}){
			printf STDERR @args ;
		}
		else {
			printf STDERR @args ;
		}
	}
}


1;
__END__
# Below is stub documentation for your module. You'd better edit it!

=head1 NAME

Ugly - Perl extension for blah blah blah

=head1 SYNOPSIS

  use Ugly;
  blah blah blah

=head1 DESCRIPTION

Stub documentation for Ugly, created by h2xs. It looks like the
author of the extension was negligent enough to leave the stub
unedited.

Blah blah blah.

=head2 EXPORT

None by default.



=head1 SEE ALSO

Mention other useful documentation such as the documentation of
related modules or operating system documentation (such as man pages
in UNIX), or any relevant external documentation such as RFCs or
standards.

If you have a mailing list set up for your module, mention it here.

If you have a web site set up for your module, mention it here.

=head1 AUTHOR

Patrick LeBoutillier, E<lt>patrickl@localdomainE<gt>

=head1 COPYRIGHT AND LICENSE

Copyright (C) 2013 by Patrick LeBoutillier

This library is free software; you can redistribute it and/or modify
it under the same terms as Perl itself, either Perl version 5.16.2 or,
at your option, any later version of Perl 5 you may have available.


=cut
