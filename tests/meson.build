# The v2 conformance suite (contract.md section 9). Each test runs against the
# plugin named by UGLY_TEST_PLUGIN (default "mock"):
#
#   meson test -C build --suite conformance
#   UGLY_TEST_PLUGIN=lua meson test -C build --suite conformance
#   meson test -C build --setup valgrind --suite conformance

v2_env = environment()
foreach d : v2_plugin_dirs
  v2_env.prepend('UGLY_PLUGIN_PATH', d)
endforeach

foreach t : ['values', 'protocol', 'ownership', 'identity', 'callbacks', 'errors', 'threads', 'lifecycle']
  exe = executable('conformance_' + t, t + '.c',
    dependencies: [ugly2_dep, libm2],
  )
  test(t, exe, protocol: 'tap', env: v2_env, suite: 'conformance', depends: v2_plugins)
endforeach
