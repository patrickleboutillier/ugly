# The conformance suite (doc/contract.md section 9), run against every plugin
# built, plus pairwise tests between every two of them:
#
#   meson test -C build --suite conformance          # every plugin, plus pairwise
#   meson test -C build --suite conformance-lua      # one plugin
#   meson test -C build --suite pairwise             # every ordered pair of languages
#   meson test -C build --setup valgrind
#
# Each plugin's fixtures (its "conformance" module) live in tests/<plugin>/;
# the mock's are built in.

# What every test needs to find the plugins and the fixtures, from the build
# and source trees. Python: plain malloc so valgrind sees every allocation,
# and no __pycache__ in the source tree.
test_env_vars = {
  'UGLY_PLUGIN_PATH': ':'.join(plugin_dirs),
  'LUA_PATH': meson.current_source_dir() / 'lua' / '?.lua;;',
  'PYTHONPATH': meson.current_source_dir() / 'python',
  'PYTHONMALLOC': 'malloc',
  'PYTHONDONTWRITEBYTECODE': '1',
}
test_env_base = environment(test_env_vars)

groups = ['values', 'protocol', 'ownership', 'identity', 'callbacks', 'errors', 'threads', 'lifecycle']
group_exes = {}
foreach t : groups
  group_exes += { t: executable('conformance_' + t, t + '.c', dependencies: [ugly_dep, libm]) }
endforeach

foreach plugin : test_plugins
  env = environment(test_env_vars)
  env.set('UGLY_TEST_PLUGIN', plugin)
  foreach t : groups
    test(t + '-' + plugin, group_exes[t], protocol: 'tap', env: env,
      suite: ['conformance', 'conformance-' + plugin], depends: plugins)
  endforeach
endforeach

# Pairwise: every ordered pair of different plugins in one process (A calls B
# and back).
pairwise = executable('pairwise', 'pairwise.c', dependencies: [ugly_dep, libm])
foreach a : test_plugins
  foreach b : test_plugins
    if a != b
      env = environment(test_env_vars)
      env.set('UGLY_TEST_PLUGIN', a)
      env.set('UGLY_TEST_PEER', b)
      test('pairwise-' + a + '-' + b, pairwise, protocol: 'tap', env: env,
        suite: ['conformance', 'pairwise'], depends: plugins)
    endif
  endforeach
endforeach

# Bindings: plain python3 and lua processes using each other through "ugly"
# (attach mode, no C host). Each runs a TAP script from tests/<lang>/.
if binding_dirs.has_key('python') and binding_dirs.has_key('lua')
  env = environment(test_env_vars)
  env.prepend('PYTHONPATH', binding_dirs['python'])
  test('binding-python', pyinst, args: files('python' / 'test_binding.py'), protocol: 'tap', env: env,
    suite: 'bindings', depends: plugins + bindings)

  lua_exe = find_program('lua5.4', 'lua', required: false)
  if lua_exe.found()
    env = environment(test_env_vars)
    env.set('LUA_CPATH', binding_dirs['lua'] / '?.so;;')
    test('binding-lua', lua_exe, args: files('lua' / 'test_binding.lua'), protocol: 'tap', env: env,
      suite: 'bindings', depends: plugins + bindings)
  endif
endif
