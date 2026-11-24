# The mock plugin: a tiny language in C, for testing the core and the
# contract before any real interpreter is involved. Built as ugly_mock.so.

ugly_mock = shared_module('ugly_mock',
  'mock.c',
  name_prefix: '',
  dependencies: [ugly_dep, libm],
  install: false,
)

plugins += ugly_mock
plugin_dirs += meson.current_build_dir()
