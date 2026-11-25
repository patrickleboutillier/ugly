# Writes a file to stdout as a C string literal: perl embed.pl NAME FILE
my $name = shift ;
print "static const char *$name =\n" ;
while (<>){ chomp ; s/\\/\\\\/g ; s/"/\\"/g ; print "\t\"$_\\n\"\n" }
print "\t;\n" ;
