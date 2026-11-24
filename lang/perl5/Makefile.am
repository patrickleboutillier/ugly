lib_LTLIBRARIES = libugly_perl5.la

libugly_perl5_la_SOURCES = utils.c language.c runtime.c object.c

libugly_perl5_la_CFLAGS = $(shell perl -MExtUtils::Embed -e ccopts)
libugly_perl5_la_LDFLAGS = $(shell perl -MExtUtils::Embed -e ldopts)
libugly_perl5_la_LIBADD = $(top_builddir)/lib/c/libugly.la

# Helper code, loaded at runtime from $(UGLY_LANGUAGE_DIR)/perl5/helper/
helperdir = $(pkglibdir)/perl5/helper
dist_helper_DATA = helper/helper.pm
