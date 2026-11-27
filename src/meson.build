# libugly: the core (handle table, frames and borrows, errors, threads,
# plugin loading) and the built-in "c" language.

threads = dependency('threads')
libdl = cc.find_library('dl', required: false)
libm = cc.find_library('m', required: false)

ugly_inc = include_directories('../include')

# Only ugly_* is exported (ugly.map): the internals shared by the core's files
# aren't the process's to see, or to replace.
ugly_map = meson.current_source_dir() / 'ugly.map'

libugly = shared_library('ugly',
  'ctx.c', 'handles.c', 'values.c', 'lang.c', 'rt.c', 'protocol.c', 'host.c',
  include_directories: ugly_inc,
  c_args: ['-DUGLY_PLUGIN_DIR="@0@"'.format(ugly_plugin_dir)],
  dependencies: [threads, libdl, libm],
  link_args: ['-Wl,--version-script=' + ugly_map],
  link_depends: 'ugly.map',
  version: '2.8.0',
  install: true,
)

# For plugins and tests.
ugly_dep = declare_dependency(
  link_with: libugly,
  include_directories: ugly_inc,
  dependencies: threads,
)

install_headers('../include/ugly.h', '../include/ugly_plugin.h')

import('pkgconfig').generate(libugly,
  name: 'ugly',
  description: 'UGLY: in-process glue between programming languages',
)
