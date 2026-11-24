# UGLY (Ultimate Glue LaYer)
#
#   meson setup build
#   meson compile -C build
#   meson test -C build                        # C tests and Perl binding tests
#   meson test -C build --setup valgrind       # C tests under valgrind
#   UGLY_TEST_LANG=<lang> meson test -C build  # the same suite against another plugin
#
# Tests need no install step: test/meson.build points UGLY_LANGUAGE_DIR,
# LD_LIBRARY_PATH and PERL5LIB at the source and build trees.

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

# Each language plugin appends its target and its build directory, so the
# tests can depend on every plugin and find it with dlopen().
plugins = []
plugin_dirs = []

subdir('lib/c')
subdir('lang/perl5')
subdir('lib/perl5')
subdir('test')
