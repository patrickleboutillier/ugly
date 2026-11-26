# UGLY (Ultimate Glue LaYer)
#
#   meson setup build
#   meson compile -C build
#   meson test -C build                        # the conformance suite, pairwise tests and examples
#   meson test -C build --suite conformance-lua    # against one plugin
#   meson test -C build --setup valgrind       # everything under valgrind
#
# Tests need no install step: tests/meson.build points UGLY_PLUGIN_PATH and
# each language's module path at the build and source trees.

project('ugly', 'c',
  version: '2.5.0',
  license: 'Apache-2.0',
  meson_version: '>= 0.61',
  default_options: [
    'c_std=gnu11',
    'warning_level=1',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')

# The core in src/, plugins in plugins/<lang>/, the conformance suite in
# tests/. Plugins are installed to, and searched for in, ugly_plugin_dir
# (UGLY_PLUGIN_PATH overrides it at runtime). Each plugin's meson.build
# appends its target to 'plugins', its build directory to 'plugin_dirs', and
# its name to 'test_plugins' (the conformance suite runs against each). A
# plugin with a binding (plugins/<lang>/binding/) adds it to 'bindings' and
# 'binding_dirs'.
ugly_plugin_dir = get_option('prefix') / get_option('libdir') / 'ugly' / 'plugins'
plugins = []
plugin_dirs = []
test_plugins = ['mock']
bindings = []          # each language's binding (ugly module), and its build directory
binding_dirs = {}

subdir('src')
subdir('plugins/mock')
subdir('plugins/lua')
subdir('plugins/python')
subdir('plugins/js')
subdir('plugins/perl5')
subdir('plugins/java')
subdir('plugins/dotnet')
subdir('tests')
subdir('examples')


# meson test --setup valgrind: every test under valgrind memcheck.
#
# A test fails, via valgrind's exit status, on any memory error (invalid
# reads/writes, uninitialised values) and on any definite, indirect or
# possible leak. "Still reachable" memory is allowed: a plugin may keep
# process-wide state that outlives every language handle (CPython is never
# finalized). The tests therefore delete everything they create.
#
# --keep-debuginfo keeps stack traces readable for plugins that have been
# dlclose()d. tests/valgrind.supp suppresses only CPython's live objects
# reported as "possibly lost" (Python is never finalized); see that file. The
# JVM can't run under valgrind (it uses SIGSEGV on purpose), so suite 'jvm'
# (anything that starts it) is left out; the Java plugin's checks are -Xcheck:jni
# (UGLY_JAVA_CHECK) and the JVM's own. The same goes for the CLR, suite 'clr'.
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
      '--suppressions=' + (meson.project_source_root() / 'tests' / 'valgrind.supp'),
    ],
    exclude_suites: ['jvm', 'clr', 'haproxy'],
    timeout_multiplier: 10,
  )
  # Data races, on the threads group: meson test --setup helgrind threads-mock threads-lua ...
  add_test_setup('helgrind',
    exe_wrapper: [valgrind, '--tool=helgrind', '--error-exitcode=99', '--num-callers=30'],
    exclude_suites: ['jvm', 'clr', 'haproxy'],
    timeout_multiplier: 10,
  )
endif
