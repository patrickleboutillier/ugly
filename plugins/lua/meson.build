# The Lua 5.4 plugin, built as ugly_lua.so. Optional: -Dlua=disabled skips it.

lua_dep = dependency('lua5.4', 'lua-5.4', 'lua54', required: get_option('lua'))

if lua_dep.found()
  ugly_lua = shared_module('ugly_lua',
    'lua.c',
    name_prefix: '',
    dependencies: [ugly_dep, lua_dep],
    install: true,
    install_dir: ugly_plugin_dir,
  )
  plugins += ugly_lua
  plugin_dirs += meson.current_build_dir()
  test_plugins += 'lua'
  subdir('binding')
endif
