# The Java plugin, built as ugly_java.so, with its Java side ugly.jar next to
# it, and its binding (binding/, libugly_jni.so). Needs a JDK (javac, jni.h,
# libjvm.so). Optional: -Djava=disabled.

javac = find_program('javac', required: get_option('java'))
java_ok = javac.found() and add_languages('java', required: false, native: false)

if java_ok
  # JAVA_HOME: from the environment, else from javac's real location.
  java_home = run_command('sh', '-c', 'echo "${JAVA_HOME:-$(dirname $(dirname $(readlink -f $(command -v javac))))}"',
    check: true).stdout().strip()
  jvm_dir = java_home / 'lib' / 'server'
  jni_inc_dep = declare_dependency(
    include_directories: include_directories(java_home / 'include', java_home / 'include' / 'linux'),
  )
  jni_dep = declare_dependency(
    dependencies: jni_inc_dep,
    link_args: ['-L' + jvm_dir, '-ljvm', '-Wl,-rpath,' + jvm_dir],
  )

  ugly_jar = jar('ugly',
    'ugly/Bridge.java', 'ugly/Foreign.java', 'ugly/ForeignException.java', 'ugly/Ugly.java',
    install: true,
    install_dir: ugly_plugin_dir,
  )

  ugly_java = shared_module('ugly_java',
    'java.c',
    name_prefix: '',
    dependencies: [ugly_dep, jni_dep],
    install: true,
    install_dir: ugly_plugin_dir,
  )
  plugins += [ugly_java, ugly_jar]
  plugin_dirs += meson.current_build_dir()
  test_plugins += 'java'
  java_prog = find_program(java_home / 'bin' / 'java', 'java')
  subdir('binding')
endif
