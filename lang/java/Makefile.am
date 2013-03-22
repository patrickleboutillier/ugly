lib_LTLIBRARIES = libugly_java.la

libugly_java_la_SOURCES = utils.c language.c runtime.c object.c

libugly_java_la_CFLAGS = -I/usr/lib/jvm/java-1.7.0-openjdk-1.7.0.9.x86_64/include/ -I/usr/lib/jvm/java-1.7.0-openjdk-1.7.0.9.x86_64/include/linux/
libugly_java_la_LDFLAGS = -L../../lib/c -lugly -L/usr/lib/jvm/java-1.7.0-openjdk-1.7.0.9.x86_64/jre/lib/amd64/server -ljvm
