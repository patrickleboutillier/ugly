# Examples, built and run as tests (exit status) when their languages are available.

if test_plugins.contains('lua') and test_plugins.contains('python')
  polyglot = executable('polyglot', 'polyglot.c', dependencies: ugly_dep)
  test('polyglot', polyglot, env: test_env_base, suite: 'examples', depends: plugins)
endif
