# libugly: the C core.

# The plugin vtable, generated from the UGLY_IFACE lines of the public API.
iface_h = custom_target('ugly_iface.h',
  input: ['gen_iface.sh', 'ugly_api.h'],
  output: 'ugly_iface.h',
  command: [find_program('bash'), '@INPUT0@', '@INPUT1@'],
  capture: true,
)

libdl = cc.find_library('dl', required: false)

libugly = shared_library('ugly',
  'ugly.c', 'context.c', 'value.c', 'language.c', 'runtime.c', 'object.c',
  iface_h,
  c_args: ['-DUGLY_LANGUAGE_DIR="@0@"'.format(pkglibdir)],
  dependencies: libdl,
  version: '0.0.0',
  install: false,     # v1 is frozen and no longer installed (see the top-level meson.build)
)

# For plugins, bindings and tests built here. It exposes the private headers
# too (ugly_priv.h, ugly_iface.h): plugins and bindings use them.
libugly_dep = declare_dependency(
  link_with: libugly,
  include_directories: include_directories('.'),
  sources: iface_h,
)
