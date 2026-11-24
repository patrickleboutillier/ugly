# The v2 conformance suite (contract.md section 9). Each test runs against the
# plugin named by UGLY_TEST_PLUGIN (default "mock"):
#
#   meson test -C build --suite conformance          # against every plugin built
#   meson test -C build --suite conformance-lua      # against one plugin
#   meson test -C build --setup valgrind --suite conformance

# Each plugin's fixtures live in tests/<plugin>/ (the mock's are built in).
foreach plugin : v2_test_plugins
  env = environment()
  env.set('UGLY_TEST_PLUGIN', plugin)
  foreach d : v2_plugin_dirs
    env.prepend('UGLY_PLUGIN_PATH', d)
  endforeach
  env.set('LUA_PATH', meson.current_source_dir() / 'lua' / '?.lua;;')

  foreach t : ['values', 'protocol', 'ownership', 'identity', 'callbacks', 'errors', 'threads', 'lifecycle']
    exe = 'conformance_' + t
    if not is_variable(exe)
      set_variable(exe, executable(exe, t + '.c', dependencies: [ugly2_dep, libm2]))
    endif
    test(t + '-' + plugin, get_variable(exe), protocol: 'tap', env: env,
      suite: ['conformance', 'conformance-' + plugin], depends: v2_plugins)
  endforeach
endforeach

# Pairwise: every ordered pair of different plugins in one process (A calls B
# and back). Fixtures for both come from tests/<plugin>/.
pairwise = executable('pairwise', 'pairwise.c', dependencies: [ugly2_dep, libm2])
foreach a : v2_test_plugins
  foreach b : v2_test_plugins
    if a != b
      env = environment()
      env.set('UGLY_TEST_PLUGIN', a)
      env.set('UGLY_TEST_PEER', b)
      foreach d : v2_plugin_dirs
        env.prepend('UGLY_PLUGIN_PATH', d)
      endforeach
      env.set('LUA_PATH', meson.current_source_dir() / 'lua' / '?.lua;;')
      test('pairwise-' + a + '-' + b, pairwise, protocol: 'tap', env: env,
        suite: ['conformance', 'pairwise'], depends: v2_plugins)
    endif
  endforeach
endforeach
