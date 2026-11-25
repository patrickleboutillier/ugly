# The Java binding: java.c again, as libugly_jni.so, which ugly.Foreign loads
# (System.loadLibrary("ugly_jni")) in a plain java program, so its JNI_OnLoad
# attaches UGLY to that JVM. It doesn't link libjvm: the JVM is the host's.
# Installed in libdir, which is on Debian's default java.library.path; the
# Java side is ugly.jar, in the plugin directory.

java_binding = shared_module('ugly_jni',
  '../java.c',
  c_args: '-DUGLY_BINDING',
  dependencies: [ugly_dep, jni_inc_dep],
  install: true,
  install_dir: get_option('libdir'),
)
binding_dirs += { 'java': meson.current_build_dir() }
bindings += java_binding
