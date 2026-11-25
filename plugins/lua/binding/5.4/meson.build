# The Lua binding for one Lua version (binding_lua_version, set by
# ../meson.build); the 5.3 and 5.4 directories hold the same file.

lua_binding = shared_module('ugly',
  '../../lua.c',
  name_prefix: '',
  c_args: '-DUGLY_BINDING',
  dependencies: [ugly_dep, binding_lua_dep.partial_dependency(compile_args: true, includes: true)],
  install: true,
  install_dir: binding_lua_dep.get_variable(pkgconfig: 'INSTALL_CMOD',
    default_value: get_option('prefix') / get_option('libdir') / 'lua' / binding_lua_version),
)
binding_dirs += { 'lua' + binding_lua_version: meson.current_build_dir() }
bindings += lua_binding
