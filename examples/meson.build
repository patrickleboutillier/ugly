# Examples, built and run as tests (exit status) when their languages are available.

if test_plugins.contains('lua') and test_plugins.contains('python')
  polyglot = executable('polyglot', 'polyglot.c', dependencies: ugly_dep)
  test('polyglot', polyglot, env: test_env_base, suite: 'examples', depends: plugins)
endif

# A plain Lua script using Python's standard library through the Lua binding.
if binding_dirs.has_key('lua') and test_plugins.contains('python')
  lua_exe = find_program('lua5.4', 'lua', required: false)
  if lua_exe.found()
    env = environment(test_env_vars)
    env.set('LUA_CPATH', binding_dirs['lua'] / '?.so;;')
    test('python_from_lua', lua_exe, args: files('python_from_lua.lua'), env: env,
      suite: 'examples', depends: plugins + bindings)
  endif
endif
