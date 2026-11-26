# music-diatonic, a Haskell library from Hackage (subprojects/music-diatonic.wrap),
# used from Python and from Lua through their bindings. The library and its
# export module (Music/Diatonic/Ugly.hs) are one module library,
# Music.Diatonic.so, built like any other: against the package 'ugly', with
# a unit of its own. The library's 2015 sources warn a lot: -w.
music_diatonic = subproject('music-diatonic', required: false)
if music_diatonic.found()
  music_diatonic_so = custom_target('Music.Diatonic.so',
    input: [music_diatonic.get_variable('sources'), 'Music' / 'Diatonic' / 'Ugly.hs'],
    output: 'Music.Diatonic.so',
    command: ghc_module + ['-w', '-this-unit-id', 'music-diatonic-ugly'],
    depends: ghc_module_depends,
  )
  env = environment(test_env_vars)
  env.set('UGLY_HASKELL_PATH', meson.current_build_dir())
  if binding_dirs.has_key('python')
    env_py = env
    env_py.prepend('PYTHONPATH', binding_dirs['python'])
    test('music_from_python', pyinst, args: files('music.py'), env: env_py,
      suite: ['examples', 'ghc'], depends: plugins + bindings + [music_diatonic_so])
  endif
  if binding_dirs.has_key('lua')
    lua_exe = find_program('lua' + lua_version, required: false)
    if lua_exe.found()
      env_lua = env
      env_lua.set('LUA_CPATH', binding_dirs['lua'] / '?.so;;')
      test('music_from_lua', lua_exe, args: files('music.lua'), env: env_lua,
        suite: ['examples', 'ghc'], depends: plugins + bindings + [music_diatonic_so])
    endif
  endif
endif
