# The CPython plugin, built as ugly_python.so. Optional: -Dpython=disabled skips it.

python_dep = dependency('python3-embed', required: get_option('python'))

if python_dep.found()
  pyinst = import('python').find_installation('python3')
  ugly_python = shared_module('ugly_python',
    'python.c',
    name_prefix: '',
    dependencies: [ugly_dep, python_dep],
    install: true,
    install_dir: ugly_plugin_dir,
  )
  plugins += ugly_python
  plugin_dirs += meson.current_build_dir()
  test_plugins += 'python'

  # A process that forks after Python started: each child can call into it.
  test('python-fork', executable('python_fork', 'fork.c', dependencies: ugly_dep),
    env: {'UGLY_PLUGIN_PATH': meson.current_build_dir(), 'PYTHONMALLOC': 'malloc', 'PYTHONDONTWRITEBYTECODE': '1'},
    depends: ugly_python,
    protocol: 'tap',
    suite: ['python'],
  )
  subdir('binding')
endif
