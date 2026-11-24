SUBDIRS = c

# The Perl XS binding is built by ExtUtils::MakeMaker, which owns
# perl5/Makefile. It links against c/.libs/libugly, so it is built after c/.
perl5/Makefile: perl5/Makefile.PL
	cd perl5 && $(PERL) Makefile.PL

all-local: perl5/Makefile
	$(MAKE) -C perl5

clean-local:
	-test -f perl5/Makefile && $(MAKE) -C perl5 realclean
