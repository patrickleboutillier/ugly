# The UGLY test suite. Every test is a TAP stream.
#
#   meson test -C build                          # everything, against the perl5 plugin
#   UGLY_TEST_LANG=<lang> meson test -C build    # the same suite against another plugin
#   meson test -C build object                   # one test
#   meson test -C build --suite c                # only the C tests
#   meson test -C build --setup valgrind         # the C tests under valgrind
#
# Full output is in build/meson-logs/testlog.txt. UGLY_DEBUG=<n> adds trace
# output.

libm = cc.find_library('m', required: false)

# Plugins are found by dlopen() through LD_LIBRARY_PATH, and their helper code
# under UGLY_LANGUAGE_DIR/<lang>/. Test libraries (calc, oocalc) live in
# test/<lang>/. PERL5LIB serves both the embedded perl and the binding tests:
# Ugly.pm from the source tree, auto/Ugly/Ugly.so from the build tree.
test_env = environment()
test_env.set('UGLY_LANGUAGE_DIR', meson.project_source_root() / 'lang')
foreach d : plugin_dirs
  test_env.prepend('LD_LIBRARY_PATH', d)
endforeach
test_env.prepend('PERL5LIB',
  meson.current_source_dir() / 'perl5',
  meson.project_source_root() / 'lib' / 'perl5' / 'lib',
  meson.project_build_root() / 'lib' / 'perl5',
)

# Every test needs the plugins, and the perl5 plugin's helper loads the XS
# binding, even when the C API is what's under test.
test_depends = plugins + [ugly_xs]

# The C API, run against the plugin selected by UGLY_TEST_LANG.
foreach t : ['context', 'value', 'language', 'runtime', 'object']
  exe = executable(t, 't' / t + '.c',
    dependencies: [libugly_dep, libm],
  )
  test(t, exe, protocol: 'tap', env: test_env, suite: 'c', depends: test_depends)
endforeach

# The Perl binding, also driving that plugin.
foreach t : ['01_context', '02_value', '03_language', '04_runtime']
  test(t, perl,
    args: files('..' / 'lib' / 'perl5' / 't' / t + '.t'),
    protocol: 'tap', env: test_env, suite: 'perl', depends: test_depends,
  )
endforeach

# meson test --setup valgrind: the C tests under valgrind (memcheck).
#
# A test fails, via valgrind's exit status, on any memory error (invalid
# reads/writes, uninitialised values) and on any definite, indirect or
# possible leak. "Still reachable" memory is allowed: a plugin may keep
# process-wide state that outlives every language handle (perl's
# PERL_SYS_INIT3 state, which can't be torn down and re-created in the same
# process). The tests therefore delete everything they create.
#
# --keep-debuginfo keeps stack traces readable for code in plugins that
# ugly_language_delete has already dlclose()d. The Perl binding tests are left
# out: running perl itself under valgrind is mostly noise about perl.
valgrind = find_program('valgrind', required: false)
if valgrind.found()
  add_test_setup('valgrind',
    exe_wrapper: [valgrind,
      '--error-exitcode=99',
      '--leak-check=full',
      '--errors-for-leak-kinds=definite,indirect,possible',
      '--track-origins=yes',
      '--num-callers=30',
      '--keep-debuginfo=yes',
    ],
    exclude_suites: ['perl'],
    timeout_multiplier: 10,
  )
endif
