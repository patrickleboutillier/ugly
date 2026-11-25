# Examples, built and run as tests (exit status) when their languages are available.

if test_plugins.contains('lua') and test_plugins.contains('python')
  polyglot = executable('polyglot', 'polyglot.c', dependencies: ugly_dep)
  test('polyglot', polyglot, env: test_env_base, suite: 'examples', depends: plugins)
endif

# A plain Lua script using Python's standard library through the Lua binding.
if binding_dirs.has_key('lua') and test_plugins.contains('python')
  lua_exe = find_program('lua' + lua_version, required: false)
  if lua_exe.found()
    env = environment(test_env_vars)
    env.set('LUA_CPATH', binding_dirs['lua'] / '?.so;;')
    test('python_from_lua', lua_exe, args: files('python_from_lua.lua'), env: env,
      suite: 'examples', depends: plugins + bindings)
  endif
endif

# HAProxy asks Python for a decision on each request (examples/haproxy/), when
# haproxy is installed and its Lua has a binding here. One thread with
# lua-load, and four with lua-load-per-thread (a Lua state per worker thread,
# one Python). Not under valgrind: the test wraps the runner, not haproxy.
haproxy = find_program('haproxy', '/usr/sbin/haproxy', required: false)
if haproxy.found() and test_plugins.contains('python')
  haproxy_lua = run_command(haproxy, '-vv', check: false).stdout()
  haproxy_lua_version = ''
  foreach v : ['5.4', '5.3']
    if haproxy_lua.contains('Built with Lua version : Lua ' + v)
      haproxy_lua_version = v
    endif
  endforeach
  if haproxy_lua_version != '' and binding_dirs.has_key('lua' + haproxy_lua_version)
    env = environment(test_env_vars)
    env.set('LUA_CPATH', binding_dirs['lua' + haproxy_lua_version] / '?.so;;')
    foreach mode : [['1', 'lua-load'], ['4', 'lua-load-per-thread']]
      test('haproxy-poc-' + mode[1], pyinst, args: [files('haproxy' / 'run_poc.py'), haproxy.full_path(), mode[0], mode[1]],
        protocol: 'tap', env: env, suite: ['examples', 'haproxy'], depends: plugins + bindings, is_parallel: false)
    endforeach
  endif
endif
