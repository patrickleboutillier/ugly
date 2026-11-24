# libugly v2: the core (handle table, frames and borrows, errors, threads,
# plugin loading) and the built-in "c" language.

threads = dependency('threads')
libdl2 = cc.find_library('dl', required: false)
libm2 = cc.find_library('m', required: false)

ugly_inc = include_directories('../include')

libugly2 = shared_library('ugly',
  'ctx.c', 'handles.c', 'values.c', 'lang.c', 'rt.c', 'protocol.c', 'host.c',
  include_directories: ugly_inc,
  c_args: ['-DUGLY_PLUGIN_DIR="@0@"'.format(ugly_plugin_dir)],
  dependencies: [threads, libdl2, libm2],
  version: '2.0.0',
  install: true,
)

# For plugins and tests.
ugly2_dep = declare_dependency(
  link_with: libugly2,
  include_directories: ugly_inc,
  dependencies: threads,
)

install_headers('../include/ugly.h', '../include/ugly_plugin.h')

import('pkgconfig').generate(libugly2,
  name: 'ugly',
  description: 'UGLY: in-process glue between programming languages',
)
