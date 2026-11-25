# The Lua plugin, built as ugly_lua.so against one Lua: -Dlua_version=auto
# takes 5.4, else 5.3. A process holds one Lua (every version exports the same
# lua_* symbols), so the language is "lua" whatever the version; code that
# cares reads _VERSION. The binding is built for every Lua found
# (binding/<version>/ugly.so), since each loads only into its own host's Lua.
# Optional: -Dlua=disabled skips both.

lua_deps = {}        # version -> dependency, for every Lua found
lua_version = ''     # the plugin's
if not get_option('lua').disabled()
  foreach v : ['5.4', '5.3']
    d = dependency('lua' + v, 'lua-' + v, 'lua' + v.replace('.', ''), required: false)
    if d.found()
      lua_deps += { v: d }
    endif
  endforeach
  want = get_option('lua_version')
  if want == 'auto'
    foreach v : ['5.4', '5.3']
      if lua_version == '' and lua_deps.has_key(v)
        lua_version = v
      endif
    endforeach
  elif lua_deps.has_key(want)
    lua_version = want
  else
    error('-Dlua_version=@0@: no Lua @0@ development files (liblua@0@-dev)'.format(want))
  endif
  if lua_version == '' and get_option('lua').enabled()
    error('the lua plugin needs liblua5.4-dev or liblua5.3-dev')
  endif
endif

if lua_version != ''
  lua_dep = lua_deps[lua_version]
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
