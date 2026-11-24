# The Lua binding: lua.c again, as the Lua C module "ugly" (ugly.so), for
# require "ugly" from a plain lua process. It doesn't link liblua: its Lua
# symbols come from the host interpreter (a second Lua in the process would
# be fatal).

lua_binding = shared_module('ugly',
  '../lua.c',
  name_prefix: '',
  c_args: '-DUGLY_BINDING',
  dependencies: [ugly_dep, lua_dep.partial_dependency(compile_args: true, includes: true)],
  install: true,
  install_dir: lua_dep.get_variable(pkgconfig: 'INSTALL_CMOD',
    default_value: get_option('prefix') / get_option('libdir') / 'lua' / '5.4'),
)
binding_dirs += { 'lua': meson.current_build_dir() }
bindings += lua_binding
