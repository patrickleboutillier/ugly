lib_LTLIBRARIES = libugly_perl5.la

libugly_perl5_la_SOURCES = utils.c language.c runtime.c 

libugly_perl5_la_CFLAGS = $(shell perl -MExtUtils::Embed -e ccopts)
libugly_perl5_la_LDFLAGS = $(shell perl -MExtUtils::Embed -e ldopts) -L../../lib/c -lugly
