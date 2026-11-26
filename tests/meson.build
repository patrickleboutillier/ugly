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
# Java fixtures: a jar on UGLY_JAVA_CLASSPATH (they use the ugly package).
java_fixture_cp = ''
if test_plugins.contains('java')
  conformance_jar = jar('conformance', 'java' / 'conformance.java', link_with: ugly_jar)
  java_fixture_cp = conformance_jar.full_path()
  plugins += conformance_jar
endif

# C# fixtures: conformance.dll on UGLY_DOTNET_PATH (it references the plugin's Ugly.dll).
dotnet_fixture_dir = ''
if test_plugins.contains('dotnet')
  subdir('dotnet')
endif

# Haskell fixtures: conformance.so on UGLY_HASKELL_PATH (built against the plugin's package).
haskell_fixture_dir = ''
if test_plugins.contains('haskell')
  subdir('haskell')
endif

test_env_vars = {
  'UGLY_PLUGIN_PATH': ':'.join(plugin_dirs),
  'LUA_PATH': meson.current_source_dir() / 'lua' / '?.lua;;',
  'PYTHONPATH': meson.current_source_dir() / 'python',
  'PYTHONMALLOC': 'malloc',
  'PYTHONDONTWRITEBYTECODE': '1',
  'UGLY_JS_PATH': meson.current_source_dir() / 'js',
  'PERL5LIB': meson.current_source_dir() / 'perl5',
  'UGLY_JAVA_CLASSPATH': java_fixture_cp,
  'UGLY_DOTNET_PATH': dotnet_fixture_dir,
  'UGLY_HASKELL_PATH': haskell_fixture_dir,
  # Modules found only through ugly_configure (binding tests).
  'UGLY_TEST_CONFIGURED': meson.current_source_dir() / 'configured',
  # The plugins built: binding tests skip what needs one that isn't.
  'UGLY_TEST_LANGS': ','.join(test_plugins),
  # -Xcheck:jni: the JVM checks the plugin's JNI use (its warnings break the TAP stream, failing the test).
  'UGLY_JAVA_CHECK': '1',
}
test_env_base = environment(test_env_vars)

groups = ['values', 'protocol', 'ownership', 'identity', 'callbacks', 'errors', 'threads', 'lifecycle', 'options']
group_exes = {}
foreach t : groups
  group_exes += { t: executable('conformance_' + t, t + '.c', dependencies: [ugly_dep, libm]) }
endforeach

foreach plugin : test_plugins
  env = environment(test_env_vars)
  env.set('UGLY_TEST_PLUGIN', plugin)
  foreach t : groups
    # The JVM and the CLR can't run under valgrind (they raise SIGSEGV on purpose): suites 'jvm' and 'clr' are left out there.
    # GHC's RTS runs under valgrind without origins only: suite 'ghc' (setup valgrindghc).
    test(t + '-' + plugin, group_exes[t], protocol: 'tap', env: env,
      suite: ['conformance', 'conformance-' + plugin] + (plugin == 'java' ? ['jvm'] : []) + (plugin == 'dotnet' ? ['clr'] : [])
        + (plugin == 'haskell' ? ['ghc'] : []),
      depends: plugins)
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
        suite: ['conformance', 'pairwise'] + (a == 'java' or b == 'java' ? ['jvm'] : [])
          + (a == 'dotnet' or b == 'dotnet' ? ['clr'] : []) + (a == 'haskell' or b == 'haskell' ? ['ghc'] : []),
        depends: plugins)
    endif
  endforeach
endforeach

# Benchmarks (meson test -C build --benchmark --verbose): every language
# built, in one process, printing Markdown tables (tests/bench.c).
# Without -Xcheck:jni, which slows every JNI call.
bench_env = environment(test_env_vars)
bench_env.set('UGLY_JAVA_CHECK', '')
benchmark('bench', executable('bench', 'bench.c', dependencies: [ugly_dep, libm, dependency('threads')]),
  env: bench_env, timeout: 1800, depends: plugins)

# Bindings: plain python3 and lua processes using each other through "ugly"
# (attach mode, no C host). Each runs a TAP script from tests/<lang>/.
if binding_dirs.has_key('python') and binding_dirs.has_key('lua')
  env = environment(test_env_vars)
  env.prepend('PYTHONPATH', binding_dirs['python'])
  test('binding-python', pyinst, args: files('python' / 'test_binding.py'), protocol: 'tap', env: env,
    suite: 'bindings', depends: plugins + bindings)

  if binding_dirs.has_key('js') and is_variable('qjs_exe')
    env = environment(test_env_vars)
    env.set('UGLY_JS_BINDING', binding_dirs['js'] / 'ugly.so')
    test('binding-js', qjs_exe, args: files('js' / 'test_binding.js'), protocol: 'tap', env: env,
      suite: 'bindings', depends: plugins + bindings)
  endif

  if binding_dirs.has_key('perl5')
    env = environment(test_env_vars)
    env.set('PERL5LIB', ':'.join([perl_binding_lib, binding_dirs['perl5'], meson.current_source_dir() / 'perl5']))
    # A perl program skips freeing its memory at exit; level 2 makes it free everything, so
    # the valgrind setup sees only what UGLY leaks (nothing, at the time of writing).
    env.set('PERL_DESTRUCT_LEVEL', '2')
    test('binding-perl5', perl_prog, args: files('perl5' / 'test_binding.pl'), protocol: 'tap', env: env,
      suite: 'bindings', depends: plugins + bindings)
  endif

  # The JVM configured from Python: class path and JVM options (ugly_configure).
  if test_plugins.contains('java')
    env = environment(test_env_vars)
    env.prepend('PYTHONPATH', binding_dirs['python'])
    env.set('UGLY_JAVA_CLASSPATH', '')
    env.set('UGLY_TEST_JAR', java_fixture_cp)
    test('configure-java', pyinst, args: files('java' / 'test_configure.py'), protocol: 'tap', env: env,
      suite: ['bindings', 'jvm'], depends: plugins + bindings)
  endif

  # A plain java program; teardown at JVM exit prints the last test. Its VM
  # output goes to stderr, out of the TAP stream.
  if binding_dirs.has_key('java') and binding_dirs.has_key('js') and binding_dirs.has_key('perl5')
    binding_jar = jar('test_binding', 'java' / 'test_binding.java', link_with: ugly_jar)
    test('binding-java', java_prog,
      args: ['-Xcheck:jni', '-XX:+DisplayVMOutputToStderr', '-Djava.library.path=' + binding_dirs['java'],
        '-cp', ugly_jar.full_path() + ':' + binding_jar.full_path(), 'test_binding'],
      protocol: 'tap', env: environment(test_env_vars),
      suite: ['bindings', 'jvm'], depends: plugins + bindings + [binding_jar])
  endif

  # A plain .NET program (dotnet test_binding.dll): the binding is Ugly.dll, which finds
  # libugly through UGLY_LIBRARY. Teardown (Interop.Shutdown) prints the last test.
  if test_plugins.contains('dotnet')
    env = environment(test_env_vars)
    env.set('UGLY_LIBRARY', libugly.full_path())
    test('binding-dotnet', dotnet, args: [test_binding_dll.full_path()], protocol: 'tap', env: env,
      suite: ['bindings', 'clr'], depends: plugins + bindings + [test_binding_dll])
  endif

  # The Lua binding, once per Lua it was built for, under that Lua's own interpreter.
  foreach v, d : lua_deps
    lua_exe = find_program('lua' + v, required: false)
    if lua_exe.found()
      env = environment(test_env_vars)
      env.set('LUA_CPATH', binding_dirs['lua' + v] / '?.so;;')
      test('binding-lua' + v, lua_exe, args: files('lua' / 'test_binding.lua'), protocol: 'tap', env: env,
        suite: 'bindings', depends: plugins + bindings)
    endif
  endforeach
endif

# The Haskell binding: a Haskell program as the host (tests/haskell/TestBinding.hs),
# using the languages built (UGLY_TEST_LANGS; the others are skipped).
if test_plugins.contains('haskell')
  # Meson 0.61's test() takes no custom target: env runs it (valgrind follows the exec).
  test('binding-haskell', find_program('env'), args: [haskell_test_binding], protocol: 'tap', env: test_env_base,
    suite: ['bindings', 'ghc'], depends: plugins + bindings + [haskell_test_binding])
endif
