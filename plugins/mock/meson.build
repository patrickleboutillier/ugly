# The mock plugin: a tiny language in C, for testing the core and the
# contract before any real interpreter is involved. Built as ugly_mock.so.

ugly_mock = shared_module('ugly_mock',
  'mock.c',
  name_prefix: '',
  dependencies: [ugly2_dep, libm2],
  install: false,
)

v2_plugins += ugly_mock
v2_plugin_dirs += meson.current_build_dir()
