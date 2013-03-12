lib_LTLIBRARIES = libugly.la

include_HEADERS = ugly.h

libugly_la_SOURCES = ugly.c context.c value.c language.c runtime.c 

BUILT_SOURCES = ugly_iface.h
CLEANFILES = ugly_iface.h
EXTRA_DIST = gen_iface.sh

ugly_iface.h: gen_iface.sh ugly_api.h
	./gen_iface.sh > ugly_iface.h

