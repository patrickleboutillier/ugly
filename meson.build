# UGLY (Ultimate Glue LaYer)
#
#   meson setup build
#   meson compile -C build
#   meson test -C build                            # everything: v2 conformance, v1 C and Perl tests
#   meson test -C build --suite conformance        # v2 only
#   meson test -C build --setup valgrind           # C tests (v2 and v1) under valgrind
#   UGLY_TEST_PLUGIN=<lang> meson test -C build --suite conformance   # v2 suite against another plugin
#
# Tests need no install step: tests/meson.build and test/meson.build point
# the plugin search paths at the build tree.

project('ugly', 'c',
  version: '0.02',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 0.61',
  default_options: [
    'c_std=gnu11',
    'warning_level=1',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')
perl = find_program('perl')

# Plugin helper code is installed to, and looked up at runtime under,
# <pkglibdir>/<lang>/helper/. UGLY_LANGUAGE_DIR overrides it at runtime.
pkglibdir = get_option('prefix') / get_option('libdir') / 'ugly'

# v2: the core in src/, plugins in plugins/<lang>/, the conformance suite in
# tests/. Plugins are installed to, and searched for in, ugly_plugin_dir
# (UGLY_PLUGIN_PATH overrides it at runtime).
ugly_plugin_dir = get_option('prefix') / get_option('libdir') / 'ugly' / 'plugins'
v2_plugins = []
v2_plugin_dirs = []
v2_test_plugins = ['mock']     # the conformance suite runs against each of these

subdir('src')
subdir('plugins/mock')
subdir('plugins/lua')
subdir('tests')

# v1 (frozen; the reference is tag v0.02): built and tested, but not
# installed, until Perl is ported to v2. Each v1 language plugin appends its
# target and its build directory, so the tests can depend on every plugin and
# find it with dlopen().
plugins = []
plugin_dirs = []

subdir('lib/c')
subdir('lang/perl5')
subdir('lib/perl5')
subdir('test')


# meson test --setup valgrind: the C tests (v2 conformance and v1) under
# valgrind memcheck.
#
# A test fails, via valgrind's exit status, on any memory error (invalid
# reads/writes, uninitialised values) and on any definite, indirect or
# possible leak. "Still reachable" memory is allowed: a plugin may keep
# process-wide state that outlives every language handle (perl's
# PERL_SYS_INIT3 state, which can't be torn down and re-created in the same
# process). The tests therefore delete everything they create.
#
# --keep-debuginfo keeps stack traces readable for plugins that have been
# dlclose()d. The v1 Perl binding tests are left out: running perl itself
# under valgrind is mostly noise about perl.
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
